#include <QtTest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTcpServer>
#include <QTcpSocket>

// This HTTP peer tests transport/review behavior; it is never a production provider.
class CliHttpPeer final : public QObject {
public:
    QTcpServer server;
    QPointer<QTcpSocket> socket;
    QByteArray received;
    CliHttpPeer() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            received.clear();
            socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this, [this] { received += socket->readAll(); });
        });
    }
    QString endpoint() const { return QString("http://127.0.0.1:%1/v1/chat/completions").arg(server.serverPort()); }
    bool ready() const {
        const auto split = received.indexOf("\r\n\r\n");
        const auto length = received.toLower().indexOf("content-length:");
        if (split < 0 || length < 0) return false;
        const auto end = received.indexOf("\r\n", length);
        return received.size() - split - 4 >= received.mid(length + 15, end - length - 15).trimmed().toInt();
    }
    QJsonObject requestBody() const { return QJsonDocument::fromJson(received.mid(received.indexOf("\r\n\r\n") + 4)).object(); }
    void propose(const QJsonObject &arguments) {
        const QJsonObject call{{"index", 0}, {"id", "test-call"}, {"type", "function"},
            {"function", QJsonObject{{"name", "submit_proposal"}, {"arguments", QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact))}}}};
        const QJsonObject delta{{"tool_calls", QJsonArray{call}}};
        const QJsonObject choice{{"index", 0}, {"delta", delta}, {"finish_reason", "tool_calls"}};
        const QJsonObject chunk{{"choices", QJsonArray{choice}}};
        const auto body = "data: " + QJsonDocument(chunk).toJson(QJsonDocument::Compact) + "\n\ndata: [DONE]\n\n";
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
};
class CliAgentTests final : public QObject {
    Q_OBJECT
    QList<QJsonObject> events_;
    QString program() const { return QCoreApplication::applicationDirPath() + "/rose-cli"
#ifdef Q_OS_WIN
        ".exe"
#endif
        ; }
    void start(QProcess &process, const QTemporaryDir &dir) {
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("XDG_CONFIG_HOME", dir.path());
        environment.insert("HOME", dir.path());
        process.setProcessEnvironment(environment);
        process.start(program(), {"session", "--allow-root", dir.path()});
    }
    void send(QProcess &process, const QString &id, const QString &method, QJsonObject params = {}) {
        process.write(QJsonDocument(QJsonObject{{"id", id}, {"method", method}, {"params", params}}).toJson(QJsonDocument::Compact) + '\n');
    }
    QJsonObject readReply(QProcess &process, const QString &id) {
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 10000) {
            while (process.canReadLine()) {
                const auto object = QJsonDocument::fromJson(process.readLine()).object();
                if (object.value("type") == "agentEvent") events_.append(object);
                else if (object.value("id") == id) return object;
            }
            QTest::qWait(5);
        }
        return {};
    }
    QJsonObject request(QProcess &process, const QString &id, const QString &method, QJsonObject params = {}) {
        send(process, id, method, std::move(params));
        return readReply(process, id);
    }
    QJsonObject review(QProcess &process) {
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 10000) {
            for (const auto &event : events_) if (event.value("proposal").isObject()) return event;
            while (process.canReadLine()) {
                const auto object = QJsonDocument::fromJson(process.readLine()).object();
                if (object.value("type") == "agentEvent") events_.append(object);
            }
            QTest::qWait(5);
        }
        return {};
    }
    QJsonObject configure(QProcess &process, const CliHttpPeer &peer) {
        return request(process, "configure", "agent.configure", {{"kind", "local"}, {"endpoint", peer.endpoint()}, {"model", "transport-test"}});
    }
    QString rootId(const QJsonObject &projection) const {
        for (const auto &element : projection.value("elements").toArray())
            if (element.toObject().value("kind") == "Class_Category") return element.toObject().value("id").toString();
        return {};
    }
private slots:
    void init() {
#ifndef Q_OS_LINUX
        QSKIP("These subprocess tests isolate NativeFormat settings using XDG_CONFIG_HOME on Linux");
#endif
        events_.clear();
    }
    void failedOutputDropsUnprocessedSave() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QProcess process;
        process.setStandardOutputFile("/dev/full");
        start(process, dir); QVERIFY(process.waitForStarted());
        const auto path = dir.filePath("must-not-save.mdl");
        const auto create = QJsonDocument(QJsonObject{{"id", "create"}, {"method", "create"}, {"params", QJsonObject{{"file", path}}}}).toJson(QJsonDocument::Compact);
        const auto save = QJsonDocument(QJsonObject{{"id", "save"}, {"method", "save"}, {"params", QJsonObject{}}}).toJson(QJsonDocument::Compact);
        process.write(create + '\n' + save + '\n');
        process.closeWriteChannel();
        QVERIFY(process.waitForFinished());
        QCOMPARE(process.exitCode(), 1);
        QVERIFY(!QFile::exists(path));
    }
    void inputAndManualInspectionRemainLiveDuringNetworkIO() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        CliHttpPeer peer; QVERIFY(peer.server.isListening());
        QProcess process; start(process, dir); QVERIFY(process.waitForStarted());
        QVERIFY(configure(process, peer).value("ok").toBool());
        const auto path = dir.filePath("unsaved.mdl");
        QVERIFY(request(process, "create", "create", {{"file", path}}).value("ok").toBool());
        const auto started = request(process, "start", "agent.start", {{"prompt", "Design an order service"}, {"selectedIds", QJsonArray{}}});
        QVERIFY(started.value("ok").toBool());
        QTRY_VERIFY(peer.ready());
        QVERIFY(request(process, "inspect", "inspect", {{"kind", "modelTree"}}).value("ok").toBool());
        QVERIFY(request(process, "cancel", "agent.cancel").value("ok").toBool());
        QVERIFY(!request(process, "close", "close").value("ok").toBool());
        process.closeWriteChannel(); QVERIFY(process.waitForFinished()); QCOMPARE(process.exitCode(), 0);
        QVERIFY(!QFile::exists(path));
        bool cancelled = false;
        for (const auto &event : events_) cancelled |= event.value("status") == "Cancelled";
        QVERIFY(cancelled);
    }
    void exactReviewRejectApplySaveAndReopen() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); CliHttpPeer peer;
        QProcess process; start(process, dir); QVERIFY(process.waitForStarted());
        QVERIFY(configure(process, peer).value("ok").toBool());
        const auto path = dir.filePath("model.mdl");
        const auto created = request(process, "create", "create", {{"file", path}});
        QVERIFY(created.value("ok").toBool());
        const auto root = rootId(created.value("result").toObject()); QVERIFY(!root.isEmpty());
        const auto commands = QJsonArray{QJsonObject{{"type", "renameElement"}, {"id", root}, {"name", "Billing"}}};
        auto started = request(process, "start", "agent.start", {{"prompt", "Rename the logical root"}});
        QVERIFY(started.value("ok").toBool()); QTRY_VERIFY(peer.ready());
        peer.propose({{"baseRevision", created.value("revision")}, {"commands", commands}});
        auto displayed = review(process); QVERIFY(displayed.value("proposal").isObject());
        auto proposal = displayed.value("proposal").toObject();
        QVERIFY(request(process, "reject", "agent.reject", {{"runId", started.value("result").toObject().value("runId")}, {"proposalId", proposal.value("id")}}).value("ok").toBool());
        events_.clear();
        peer.received.clear();
        started = request(process, "start2", "agent.start", {{"prompt", "Rename the logical root"}});
        QVERIFY(started.value("ok").toBool()); QTRY_VERIFY(peer.ready());
        peer.propose({{"baseRevision", created.value("revision")}, {"commands", commands}});
        displayed = review(process); proposal = displayed.value("proposal").toObject();
        QVERIFY(!proposal.isEmpty());
        QJsonObject approval{{"runId", started.value("result").toObject().value("runId")}, {"id", proposal.value("id")},
            {"baseRevision", proposal.value("baseRevision")}, {"digest", proposal.value("digest")}};
        auto altered = approval; altered.insert("digest", QString(64, '0'));
        const auto denied = request(process, "denied", "agent.apply", altered);
        QVERIFY(!denied.value("ok").toBool()); QCOMPARE(denied.value("error").toObject().value("code").toString(), QString("staleApproval"));
        const auto applied = request(process, "approved", "agent.apply", approval);
        QVERIFY(applied.value("ok").toBool()); QVERIFY(!applied.value("result").toObject().value("transactionId").toString().isEmpty());
        QVERIFY(!QFile::exists(path));
        QVERIFY(request(process, "save", "save").value("ok").toBool());
        QVERIFY(request(process, "close", "close").value("ok").toBool());
        QVERIFY(request(process, "reopen", "open", {{"file", path}}).value("ok").toBool());
        const auto inspected = request(process, "inspect", "inspect", {{"kind", "elementsById"}, {"elements", QJsonArray{root}}});
        QCOMPARE(inspected.value("result").toObject().value("elements").toArray().first().toObject().value("name").toString(), QString("Billing"));
        process.closeWriteChannel(); QVERIFY(process.waitForFinished()); QCOMPARE(process.exitCode(), 0);
    }
    void eofCancelsUnapprovedReviewWithoutSaving() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); CliHttpPeer peer;
        QProcess process; start(process, dir); QVERIFY(process.waitForStarted());
        QVERIFY(configure(process, peer).value("ok").toBool());
        const auto path = dir.filePath("unapproved.mdl");
        const auto created = request(process, "create", "create", {{"file", path}});
        const auto root = rootId(created.value("result").toObject()); QVERIFY(!root.isEmpty());
        QVERIFY(request(process, "start", "agent.start", {{"prompt", "Rename the root"}}).value("ok").toBool());
        QTRY_VERIFY(peer.ready());
        peer.propose({{"baseRevision", created.value("revision")}, {"commands", QJsonArray{QJsonObject{{"type", "renameElement"}, {"id", root}, {"name", "NeverApproved"}}}}});
        QVERIFY(review(process).value("proposal").isObject());
        process.closeWriteChannel(); QVERIFY(process.waitForFinished()); QCOMPARE(process.exitCode(), 0);
        bool cancelled = false;
        while (process.canReadLine()) {
            const auto event = QJsonDocument::fromJson(process.readLine()).object();
            cancelled |= event.value("type") == "agentEvent" && event.value("status") == "Cancelled";
            QVERIFY(event.value("transactionId").toString().isEmpty());
        }
        QVERIFY(cancelled); QVERIFY(!QFile::exists(path));
    }
    void eofAwaitsApprovedCommitAndNeverAutosaves() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); CliHttpPeer peer;
        QProcess process; start(process, dir); QVERIFY(process.waitForStarted());
        QVERIFY(configure(process, peer).value("ok").toBool());
        const auto path = dir.filePath("pending.mdl");
        const auto created = request(process, "create", "create", {{"file", path}});
        const auto root = rootId(created.value("result").toObject()); QVERIFY(!root.isEmpty());
        const auto started = request(process, "start", "agent.start", {{"prompt", "Rename the root"}});
        QVERIFY(started.value("ok").toBool()); QTRY_VERIFY(peer.ready());
        peer.propose({{"baseRevision", created.value("revision")}, {"commands", QJsonArray{QJsonObject{{"type", "renameElement"}, {"id", root}, {"name", "Approved"}}}}});
        const auto proposal = review(process).value("proposal").toObject(); QVERIFY(!proposal.isEmpty());
        send(process, "apply", "agent.apply", {{"runId", started.value("result").toObject().value("runId")}, {"id", proposal.value("id")}, {"baseRevision", proposal.value("baseRevision")}, {"digest", proposal.value("digest")}});
        process.closeWriteChannel();
        const auto reply = readReply(process, "apply"); QVERIFY(reply.value("ok").toBool());
        const auto transaction = reply.value("result").toObject().value("transactionId").toString(); QVERIFY(!transaction.isEmpty());
        QVERIFY(process.state() == QProcess::NotRunning || process.waitForFinished());
        QCOMPARE(process.exitCode(), 0); QVERIFY(!QFile::exists(path));
        bool committed = false;
        for (const auto &event : events_) committed |= event.value("status") == "Completed" && event.value("transactionId") == transaction;
        QVERIFY(committed);
    }
    void imagesRespectCanonicalRootsAndConfigurationRejectsSecrets() {
        QTemporaryDir dir, outside; QVERIFY(dir.isValid()); QVERIFY(outside.isValid()); CliHttpPeer peer;
        QProcess process; start(process, dir); QVERIFY(process.waitForStarted());
        auto config = QJsonObject{{"kind", "local"}, {"endpoint", peer.endpoint()}, {"model", "transport-test"}, {"apiKey", "forbidden-key"}};
        QVERIFY(!request(process, "secret", "agent.configure", config).value("ok").toBool());
        QVERIFY(configure(process, peer).value("ok").toBool());
        QVERIFY(request(process, "create", "create", {{"file", dir.filePath("image.mdl")}}).value("ok").toBool());
        QImage image(2, 2, QImage::Format_ARGB32); image.fill(Qt::white);
        const auto forbidden = outside.filePath("diagram.png"); QVERIFY(image.save(forbidden));
        const auto denied = request(process, "outside", "agent.start", {{"prompt", ""}, {"images", QJsonArray{forbidden}}});
        QVERIFY(!denied.value("ok").toBool()); QCOMPARE(denied.value("error").toObject().value("code").toString(), QString("accessDenied"));
        const auto allowed = dir.filePath("diagram.png"); QVERIFY(image.save(allowed));
        QVERIFY(request(process, "image", "agent.start", {{"prompt", ""}, {"images", QJsonArray{allowed}}}).value("ok").toBool());
        QTRY_VERIFY(peer.ready());
        bool multimodal = false;
        for (const auto &message : peer.requestBody().value("messages").toArray()) {
            for (const auto &part : message.toObject().value("content").toArray()) {
                if (part.toObject().value("type") == "image_url") {
                    multimodal = part.toObject().value("image_url").toObject().value("url").toString().startsWith("data:image/png;base64,");
                }
            }
        }
        QVERIFY(multimodal);
        process.closeWriteChannel(); QVERIFY(process.waitForFinished()); QCOMPARE(process.exitCode(), 0);
    }
};
QTEST_GUILESS_MAIN(CliAgentTests)
#include "tst_cli_agent.moc"
