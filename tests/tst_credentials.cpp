#include "agent/Credentials.h"
#include "agent/AgentRun.h"
#include "core/Json.h"
#include "desktop/WorkspaceController.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMetaType>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
using namespace rose;
using namespace rose::agent;
static std::optional<WorkspaceReply> replyFor(const QSignalSpy &replies, quint64 request) {
    for (const auto &entry : replies)
        if (entry[0].toULongLong() == request) return qvariant_cast<WorkspaceReply>(entry[1]);
    return std::nullopt;
}

struct FixtureSecret { QDBusObjectPath session; QByteArray parameters, value; QString contentType; };
Q_DECLARE_METATYPE(FixtureSecret)
QDBusArgument &operator<<(QDBusArgument &arg, const FixtureSecret &value) {
    arg.beginStructure(); arg << value.session << value.parameters << value.value << value.contentType; arg.endStructure(); return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, FixtureSecret &value) {
    arg.beginStructure(); arg >> value.session >> value.parameters >> value.value >> value.contentType; arg.endStructure(); return arg;
}
class FixtureSession final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Secret.Session")
public:
    int closes = 0;
public slots:
    void Close() { ++closes; }
};
class FixtureItem final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Secret.Item")
public slots:
    FixtureSecret GetSecret(const QDBusObjectPath &session) {
        return {session, {}, QByteArray("isolated-test-marker"), "text/plain; charset=utf-8"};
    }
};
class HeldPrompt final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Secret.Prompt")
public:
    QDBusConnection bus;
    QDBusMessage pending;
    bool requested = false, active = false;
    int dismissals = 0;
    explicit HeldPrompt(const QDBusConnection &connection) : bus(connection) {}
    void release() {
        // Model a service that starts its UI only when it acknowledges Prompt.
        active = true;
        bus.send(pending.createReply());
    }
public slots:
    void Prompt(const QString &) {
        pending = message(); requested = true; active = true; setDelayedReply(true);
    }
    void Dismiss() { ++dismissals; active = false; }
};
class HeldSecretService final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Secret.Service")
public:
    QDBusConnection bus;
    QDBusMessage pending;
    bool requested = false;
    bool holdSession = true, needsPrompt = false;
    explicit HeldSecretService(const QDBusConnection &connection) : bus(connection) {}
    void release() {
        if (requested) bus.send(pending.createReply({QVariant::fromValue(QDBusVariant(QString{})),
            QVariant::fromValue(QDBusObjectPath("/fixture/session"))}));
    }
public slots:
    QDBusVariant OpenSession(const QString &, const QDBusVariant &, QDBusObjectPath &session) {
        if (holdSession) {
            pending = message(); requested = true; setDelayedReply(true);
        } else {
            session = QDBusObjectPath("/fixture/session");
        }
        return QDBusVariant(QString{});
    }
    QList<QDBusObjectPath> SearchItems(const QMap<QString, QString> &attributes, QList<QDBusObjectPath> &locked) {
        locked.clear();
        if (attributes.value("application") != "rose-agent" || attributes.value("reference") != "isolated-test") return {};
        return {QDBusObjectPath("/fixture/item")};
    }
    QList<QDBusObjectPath> Unlock(const QList<QDBusObjectPath> &paths, QDBusObjectPath &prompt) {
        prompt = QDBusObjectPath(needsPrompt ? "/fixture/prompt" : "/"); return paths;
    }
};
class CredentialsTests final : public QObject {
    Q_OBJECT
private slots:
    void heldLookupAllowsManualMutationAndCancellation() {
        const QString connectionName = "rose-credentials-fixture";
        auto bus = QDBusConnection::connectToBus(QString::fromLocal8Bit(qgetenv("DBUS_SESSION_BUS_ADDRESS")), connectionName);
        QVERIFY(bus.isConnected());
        struct Cleanup {
            QDBusConnection bus;
            QString name;
            ~Cleanup() { bus.unregisterService("org.freedesktop.secrets"); QDBusConnection::disconnectFromBus(name); }
        } cleanup{bus, connectionName};
        QVERIFY(bus.registerService("org.freedesktop.secrets"));
        qDBusRegisterMetaType<FixtureSecret>();
        HeldSecretService service(bus); FixtureSession session; FixtureItem item;
        QVERIFY(bus.registerObject("/org/freedesktop/secrets", &service, QDBusConnection::ExportAllSlots));
        QVERIFY(bus.registerObject("/fixture/session", &session, QDBusConnection::ExportAllSlots));
        QVERIFY(bus.registerObject("/fixture/item", &item, QDBusConnection::ExportAllSlots));
        auto future = Credentials::readAsync("isolated-test");
        QTRY_VERIFY(service.requested);
        QVERIFY(!future.isFinished());
        QTemporaryDir dir;
        AccessPolicy policy; policy.allowedDirectories = QStringList{dir.path()};
        desktop::WorkspaceController controller;
        QSignalSpy replies(&controller, &desktop::WorkspaceController::completed);
        const auto path = dir.filePath("manual.mdl");
        const auto created = controller.create(path, {}, policy);
        QTRY_VERIFY(replyFor(replies, created));
        const auto initial = std::get<Projection>(*replyFor(replies, created));
        ElementId owner;
        for (const auto &element : initial.elements)
            if (element.kind == "Class_Category" && element.name == "Logical View") owner = element.id;
        QVERIFY(!owner.isEmpty());
        const auto proposed = controller.propose(0, {{CreateElement{"manual", "Class", "ManualSaved", owner, {}}}});
        QTRY_VERIFY(replyFor(replies, proposed));
        const auto reply = *replyFor(replies, proposed);
        QVERIFY(std::holds_alternative<Proposal>(reply));
        const auto proposal = std::get<Proposal>(reply);
        controller.apply({proposal.id, proposal.baseRevision, proposal.digest});
        QTRY_COMPARE(controller.revision(), Revision(1));
        const auto saved = controller.save(); QTRY_VERIFY(replyFor(replies, saved));
        QVERIFY(std::holds_alternative<SaveReceipt>(*replyFor(replies, saved)));
        auto opened = Workspace::open(path, policy);
        QVERIFY(std::holds_alternative<std::unique_ptr<Workspace>>(opened));
        const auto persisted = std::get<Projection>(std::get<std::unique_ptr<Workspace>>(opened)->inspect({}));
        bool correct = false;
        for (const auto &element : persisted.elements)
            correct |= element.name == "ManualSaved" && element.id.value == proposal.newIds.value("manual");
        QVERIFY(correct);
        QVERIFY(!future.isFinished());
        future.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);
        QVERIFY(future.isCanceled());
        service.release();
        QTRY_COMPARE(session.closes, 1);
    }
    void cancelledPendingPromptIsDismissedBeforeAndAfterAcknowledgement() {
        const QString connectionName = "rose-delayed-prompt-fixture";
        auto bus = QDBusConnection::connectToBus(QString::fromLocal8Bit(qgetenv("DBUS_SESSION_BUS_ADDRESS")), connectionName);
        struct Cleanup {
            QDBusConnection bus;
            QString name;
            ~Cleanup() { bus.unregisterService("org.freedesktop.secrets"); QDBusConnection::disconnectFromBus(name); }
        } cleanup{bus, connectionName};
        QVERIFY(bus.isConnected()); QVERIFY(bus.registerService("org.freedesktop.secrets"));
        qDBusRegisterMetaType<FixtureSecret>();
        HeldSecretService service(bus); FixtureSession session; FixtureItem item; HeldPrompt prompt(bus);
        service.holdSession = false; service.needsPrompt = true;
        QVERIFY(bus.registerObject("/org/freedesktop/secrets", &service, QDBusConnection::ExportAllSlots));
        QVERIFY(bus.registerObject("/fixture/session", &session, QDBusConnection::ExportAllSlots));
        QVERIFY(bus.registerObject("/fixture/item", &item, QDBusConnection::ExportAllSlots));
        QVERIFY(bus.registerObject("/fixture/prompt", &prompt, QDBusConnection::ExportAllSlots));
        auto future = Credentials::readAsync("isolated-test");
        QTRY_VERIFY(prompt.requested); QVERIFY(prompt.active); QVERIFY(!future.isFinished());
        future.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(prompt.dismissals, 1, 1000);
        QVERIFY(!prompt.active); QVERIFY(future.isCanceled());
        QTRY_COMPARE_WITH_TIMEOUT(session.closes, 1, 1000);
        prompt.release();
        QTRY_COMPARE_WITH_TIMEOUT(prompt.dismissals, 2, 1000);
        QVERIFY(!prompt.active); QCOMPARE(session.closes, 1);
    }
    void runtimeDiscardsHeldCredentialsAfterManualRevisionChange() {
        const QString connectionName = "rose-agent-held-fixture";
        auto bus = QDBusConnection::connectToBus(QString::fromLocal8Bit(qgetenv("DBUS_SESSION_BUS_ADDRESS")), connectionName);
        struct Cleanup {
            QDBusConnection bus;
            QString name;
            ~Cleanup() { bus.unregisterService("org.freedesktop.secrets"); QDBusConnection::disconnectFromBus(name); }
        } cleanup{bus, connectionName};
        QVERIFY(bus.isConnected()); QVERIFY(bus.registerService("org.freedesktop.secrets"));
        qDBusRegisterMetaType<FixtureSecret>();
        HeldSecretService service(bus); FixtureSession session; FixtureItem item;
        QVERIFY(bus.registerObject("/org/freedesktop/secrets", &service, QDBusConnection::ExportAllSlots));
        QVERIFY(bus.registerObject("/fixture/session", &session, QDBusConnection::ExportAllSlots));
        QVERIFY(bus.registerObject("/fixture/item", &item, QDBusConnection::ExportAllSlots));
        QTemporaryDir dir; desktop::WorkspaceController controller;
        QSignalSpy replies(&controller, &desktop::WorkspaceController::completed);
        const auto created = controller.create(dir.filePath("manual.mdl"), {}, AccessPolicy{{dir.path()}, {}, "ASCII", true});
        QTRY_VERIFY(replyFor(replies, created));
        const auto initial = std::get<Projection>(*replyFor(replies, created));
        ElementId owner;
        for (const auto &element : initial.elements)
            if (element.kind == "Class_Category" && element.name == "Logical View") owner = element.id;
        QVERIFY(!owner.isEmpty());
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QPointer<QTcpSocket> socket; QByteArray request; int connections = 0;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            socket = server.nextPendingConnection(); ++connections; request.clear();
            connect(socket, &QTcpSocket::readyRead, this, [&] { request += socket->readAll(); });
        });
        ProviderConfig config;
        config.endpoint = QUrl(QString("http://127.0.0.1:%1/v1/chat/completions").arg(server.serverPort()));
        config.model = "fixture"; config.credentialReference = "isolated-test";
        AgentRun run(&controller); QSignalSpy events(&run, &AgentRun::event);
        const auto oldRun = run.start(config, {"Held lookup", {}});
        QTRY_VERIFY(service.requested); QCOMPARE(run.status(), RunStatus::Planning); QCOMPARE(connections, 0);
        const auto proposed = controller.propose(0, {{CreateElement{"manual", "Class", "ManualWork", owner, {}}}});
        QTRY_VERIFY(replyFor(replies, proposed));
        std::optional<Proposal> manual;
        for (const auto &entry : replies) if (entry[0].toULongLong() == proposed) {
            const auto value = qvariant_cast<WorkspaceReply>(entry[1]);
            if (auto proposal = std::get_if<Proposal>(&value)) manual = *proposal;
        }
        QVERIFY(manual); controller.apply({manual->id, manual->baseRevision, manual->digest});
        QTRY_COMPARE(controller.revision(), Revision(1)); QTRY_COMPARE(run.status(), RunStatus::Failed);
        QCOMPARE(qvariant_cast<RunEvent>(events.last()[0]).error->code, AgentErrorCode::StaleRevision);
        const auto saved = controller.save(); QTRY_VERIFY(replyFor(replies, saved));
        QVERIFY(std::holds_alternative<SaveReceipt>(*replyFor(replies, saved)));
        config.credentialReference.clear();
        const auto replacement = run.start(config, {"A new independent run", {}}); QVERIFY(replacement != oldRun);
        QTRY_COMPARE(connections, 1);
        QTRY_VERIFY(request.contains("\r\n\r\n"));
        QTRY_VERIFY(QJsonDocument::fromJson(request.mid(request.indexOf("\r\n\r\n") + 4)).isObject());
        QVERIFY(!request.toLower().contains("authorization:"));
        QVERIFY(!request.contains("isolated-test-marker"));
        const auto args = QJsonObject{{"baseRevision", "1"},
            {"commands", json::commands({Command{CreateElement{"replacement", "Class", "ReplacementClass", owner, {}}}})}};
        const auto message = QJsonObject{{"role", "assistant"}, {"content", QJsonValue(QJsonValue::Null)},
            {"tool_calls", QJsonArray{QJsonObject{{"id", "replacement"}, {"type", "function"},
                {"function", QJsonObject{{"name", "submit_proposal"}, {"arguments", QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}}}}};
        const auto body = QJsonDocument(QJsonObject{{"id", "fixture"}, {"choices", QJsonArray{QJsonObject{
            {"index", 0}, {"message", message}, {"finish_reason", "tool_calls"}}}}}).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size())
            + "\r\nConnection: close\r\n\r\n" + body); socket->disconnectFromHost();
        QTRY_COMPARE(run.status(), RunStatus::AwaitingReview);
        const auto displayed = run.review()->id;
        service.release(); QTRY_COMPARE_WITH_TIMEOUT(session.closes, 1, 1000);
        QCOMPARE(run.id(), replacement); QCOMPARE(run.status(), RunStatus::AwaitingReview);
        QCOMPARE(run.review()->id, displayed); QCOMPARE(connections, 1); QCOMPARE(controller.revision(), Revision(1));
        QVERIFY(run.rejectReview(displayed)); QTRY_COMPARE(run.status(), RunStatus::Completed);
    }
};
QTEST_GUILESS_MAIN(CredentialsTests)
#include "tst_credentials.moc"
