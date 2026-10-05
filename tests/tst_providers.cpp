#include "agent/Provider.h"
#include "agent/ProviderSettings.h"
#include <QSettings>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

using namespace rose::agent;

// An isolated HTTP peer verifies the transport contract, not inference/model quality.
class HttpPeer final : public QObject {
public:
    QTcpServer server;
    QByteArray received;
    QPointer<QTcpSocket> socket;
    HttpPeer() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this, [this] { received += socket->readAll(); });
        });
    }
    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1/v1/chat/completions").arg(server.serverPort())); }
    bool completeRequest() const {
        const auto end = received.indexOf("\r\n\r\n");
        if (end < 0) return false;
        const auto begin = received.toLower().indexOf("content-length:");
        if (begin < 0) return false;
        const auto lineEnd = received.indexOf("\r\n", begin);
        return received.size() - end - 4 >= received.mid(begin + 15, lineEnd - begin - 15).trimmed().toInt();
    }
    void response(const QByteArray &body, const QByteArray &status = "200 OK", const QByteArray &type = "text/event-stream") {
        socket->write("HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
};

class ProviderTests final : public QObject {
    Q_OBJECT
private slots:
    void invalidSettingsCannotReplaceWorkingConfiguration() {
        QTemporaryDir dir;
        QSettings settings(dir.filePath("provider.ini"), QSettings::IniFormat);
        ProviderConfig config; config.kind = ProviderKind::Hosted;
        config.endpoint = QUrl("https://example.test/v1/chat/completions");
        config.model = "configured-model"; config.credentialReference = "user-provider";
        QVERIFY(!ProviderSettings::store(settings, config));
        auto invalid = config; invalid.endpoint = QUrl("http://example.test/v1/chat/completions");
        QVERIFY(ProviderSettings::store(settings, invalid));
        QSettings reopened(dir.filePath("provider.ini"), QSettings::IniFormat);
        const auto loaded = ProviderSettings::load(reopened);
        QVERIFY(std::holds_alternative<ProviderConfig>(loaded));
        QCOMPARE(std::get<ProviderConfig>(loaded).endpoint, config.endpoint);
        QCOMPARE(std::get<ProviderConfig>(loaded).model, QStringLiteral("configured-model"));
        QCOMPARE(std::get<ProviderConfig>(loaded).credentialReference, QStringLiteral("user-provider"));
        reopened.setValue("agent/provider/timeoutMs", 100.5);
        QVERIFY(std::holds_alternative<AgentError>(ProviderSettings::load(reopened)));
    }
    void accumulatesInterleavedToolArguments() {
        HttpPeer peer;
        Provider provider;
        ProviderConfig config; config.endpoint = peer.url(); config.model = "fixture";
        QSignalSpy completed(&provider, &Provider::completed);
        const auto id = provider.complete(config, {{"role", "user"}, {"content", "inspect"}}, {}, {});
        QTRY_VERIFY(peer.completeRequest());
        const QByteArray body =
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_a\",\"type\":\"function\",\"function\":{\"name\":\"inspect_model\",\"arguments\":\"{\"}},{\"index\":1,\"id\":\"call_b\",\"type\":\"function\",\"function\":{\"name\":\"search_elements\",\"arguments\":\"{\\\"search\\\":\"}}]}}]}\n\n"
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":1,\"function\":{\"arguments\":\"\\\"Account\\\"}\"}},{\"index\":0,\"function\":{\"arguments\":\"}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
            "data: [DONE]\n\n";
        peer.response(body);
        QTRY_COMPARE(completed.size(), 1);
        QCOMPARE(completed.at(0).at(0).toULongLong(), id);
        const auto result = qvariant_cast<ProviderResult>(completed.at(0).at(1));
        QVERIFY(std::holds_alternative<ProviderResponse>(result));
        const auto reply = std::get<ProviderResponse>(result);
        QCOMPARE(reply.calls.size(), 2);
        QCOMPARE(reply.calls[0].id, "call_a");
        QCOMPARE(reply.calls[0].arguments, "{}");
        QCOMPARE(reply.calls[1].id, "call_b");
        QCOMPARE(reply.calls[1].arguments, "{\"search\":\"Account\"}");
    }
    void cancelSuppressesLateCompletion() {
        HttpPeer peer; Provider provider;
        ProviderConfig config; config.endpoint = peer.url(); config.model = "fixture";
        QSignalSpy completed(&provider, &Provider::completed);
        const auto id = provider.complete(config, {{"role", "user"}, {"content", "inspect"}}, {}, {});
        QTRY_VERIFY(peer.completeRequest());
        provider.cancel(id);
        QTest::qWait(20);
        peer.response("data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"late\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n");
        QTest::qWait(30);
        QCOMPARE(completed.size(), 0);
    }
    void redirectsAreNotFollowed() {
        HttpPeer peer; HttpPeer destination; Provider provider;
        ProviderConfig config; config.endpoint = peer.url(); config.model = "fixture";
        QSignalSpy completed(&provider, &Provider::completed);
        provider.complete(config, {{"role", "user"}, {"content", "inspect"}}, {}, {});
        QTRY_VERIFY(peer.completeRequest());
        peer.socket->write("HTTP/1.1 302 Found\r\nLocation: " + destination.url().toEncoded() + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        peer.socket->disconnectFromHost();
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY(std::holds_alternative<AgentError>(qvariant_cast<ProviderResult>(completed.at(0).at(1))));
        QCOMPARE(destination.received.size(), 0);
    }
    void unauthorizedDoesNotExposeBodyOrKey() {
        HttpPeer peer; Provider provider; ProviderConfig config;
        config.endpoint = peer.url(); config.model = "fixture";
        QSignalSpy completed(&provider, &Provider::completed);
        provider.complete(config, {{"role", "user"}, {"content", "inspect"}}, {}, "secret-test-token");
        QTRY_VERIFY(peer.completeRequest());
        peer.response("secret-test-token", "401 Unauthorized", "application/json");
        QTRY_COMPARE(completed.size(), 1);
        auto error = std::get<AgentError>(qvariant_cast<ProviderResult>(completed.at(0).at(1)));
        QCOMPARE(error.code, AgentErrorCode::Authentication);
        QVERIFY(!error.message.contains("secret-test-token"));
    }
    void remoteLocalHttpRejectedBeforeConnection() {
        ProviderConfig config; config.endpoint = QUrl("http://192.0.2.1/v1/chat/completions"); config.model = "fixture";
        QVERIFY(validateConfig(config).has_value());
        config.kind = ProviderKind::Hosted;
        config.endpoint = QUrl("http://127.0.0.1/v1/chat/completions");
        QVERIFY(validateConfig(config).has_value());
    }
    void truncatedStreamIsAnError() {
        HttpPeer peer; Provider provider; ProviderConfig config; config.endpoint = peer.url(); config.model = "fixture";
        QSignalSpy completed(&provider, &Provider::completed);
        provider.complete(config, {{"role", "user"}, {"content", "inspect"}}, {}, {});
        QTRY_VERIFY(peer.completeRequest());
        peer.response("data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"partial\"}}]}\n\n");
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY(std::holds_alternative<AgentError>(qvariant_cast<ProviderResult>(completed.at(0).at(1))));
    }
};
QTEST_GUILESS_MAIN(ProviderTests)
#include "tst_providers.moc"
