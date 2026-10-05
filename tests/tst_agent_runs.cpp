#include "agent/AgentRun.h"
#include "agent/AgentHistory.h"
#include "core/Json.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <array>
#endif
using namespace rose;
using namespace rose::agent;
static std::optional<WorkspaceReply> replyFor(const QSignalSpy &replies, quint64 request) {
    for (const auto &entry : replies)
        if (entry[0].toULongLong() == request) return qvariant_cast<WorkspaceReply>(entry[1]);
    return std::nullopt;
}
static std::optional<RunEvent> terminalEventFor(const QSignalSpy &events, RunId run, RunStatus status) {
    for (qsizetype i = events.size(); i > 0; --i) {
        const auto event = qvariant_cast<RunEvent>(events[i - 1][0]);
        if (event.runId == run && event.status == status && !event.assistantMessage) return event;
    }
    return std::nullopt;
}
#ifdef Q_OS_WIN
static bool privateWindowsAcl(const QString &path, bool directory) {
    auto native = QDir::toNativeSeparators(path);
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(reinterpret_cast<LPWSTR>(native.data()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &descriptor) != ERROR_SUCCESS) return false;
    const auto releaseDescriptor = qScopeGuard([&] { LocalFree(descriptor); });
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(descriptor, &control, &revision)
        || !(control & SE_DACL_PROTECTED) || !acl || acl->AceCount != 1) return false;
    void *rawAce = nullptr;
    if (!GetAce(acl, 0, &rawAce)) return false;
    const auto ace = static_cast<ACCESS_ALLOWED_ACE *>(rawAce);
    if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || ace->Mask != FILE_ALL_ACCESS
        || (directory && (ace->Header.AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE))
            != (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE))) return false;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    const auto closeToken = qScopeGuard([&] { CloseHandle(token); });
    alignas(TOKEN_USER) std::array<unsigned char, sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE> user{};
    DWORD needed = 0;
    return GetTokenInformation(token, TokenUser, user.data(), DWORD(user.size()), &needed)
        && EqualSid(&ace->SidStart, reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid);
}
#endif
static ElementId logicalOwner(const Projection &projection) {
    for (const auto &element : projection.elements)
        if (element.kind == "Class_Category" && element.name == "Logical View") return element.id;
    return {};
}

class RunPeer final : public QObject {
public:
    QTcpServer server;
    QPointer<QTcpSocket> socket;
    QByteArray bytes;
    int requests = 0;
    RunPeer() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            socket = server.nextPendingConnection(); bytes.clear(); ++requests;
            connect(socket, &QTcpSocket::readyRead, this, [this] { bytes += socket->readAll(); });
        });
    }
    bool ready() const {
        const auto end = bytes.indexOf("\r\n\r\n");
        const auto begin = bytes.toLower().indexOf("content-length:");
        if (end < 0 || begin < 0) return false;
        const auto lineEnd = bytes.indexOf("\r\n", begin);
        return bytes.size() - end - 4 >= bytes.mid(begin + 15, lineEnd - begin - 15).trimmed().toInt();
    }
    QJsonObject request() const { return QJsonDocument::fromJson(bytes.mid(bytes.indexOf("\r\n\r\n") + 4)).object(); }
    ProviderConfig config() const {
        ProviderConfig value; value.endpoint = QUrl(QString("http://127.0.0.1:%1/v1/chat/completions").arg(server.serverPort())); value.model = "fixture"; return value;
    }
    void answer(const QJsonObject &message, const QString &reason) {
        const auto body = QJsonDocument(QJsonObject{{"id", "fixture-response"}, {"choices", QJsonArray{QJsonObject{{"index", 0}, {"message", message}, {"finish_reason", reason}}}}}).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body); socket->disconnectFromHost();
    }
    void call(const QString &id, const QString &name, const QJsonObject &args) {
        const QJsonObject function{{"name", name}, {"arguments", QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}};
        const QJsonObject call{{"id", id}, {"type", "function"}, {"function", function}};
        answer({{"role", "assistant"}, {"content", QJsonValue(QJsonValue::Null)}, {"tool_calls", QJsonArray{call}}}, "tool_calls");
    }
};
static QByteArray imageBytes(const char *format, int side = 8) {
    QImage image(side, side, QImage::Format_RGB32);
    quint32 noise = 0x53A9F17BU;
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x) {
            noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
            image.setPixel(x, y, qRgb(noise & 255, (noise >> 8) & 255, (noise >> 16) & 255));
        }
    QByteArray bytes;
    QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, format)) return {};
    return bytes;
}
class AgentRunTests final : public QObject {
    Q_OBJECT
    QString originalApplicationName_, historyBase_;
    bool originalTestMode_ = false;
private slots:
    void initTestCase() {
        originalApplicationName_ = QCoreApplication::applicationName();
        originalTestMode_ = QStandardPaths::isTestModeEnabled();
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName("rose-agent-runs-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
        const auto base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        QVERIFY(!base.isEmpty()); QVERIFY(!QFileInfo::exists(base));
        historyBase_ = base;
    }
    void init() {
        if (QFileInfo::exists(historyBase_)) QVERIFY(QDir(historyBase_).removeRecursively());
    }
    void cleanupTestCase() {
        const bool removed = historyBase_.isEmpty() || !QFileInfo::exists(historyBase_) || QDir(historyBase_).removeRecursively();
        QCoreApplication::setApplicationName(originalApplicationName_);
        QStandardPaths::setTestModeEnabled(originalTestMode_);
        QVERIFY(removed);
    }
    void historyPersistsPrivateMetadataAcrossAtomicReplacements() {
        ProviderConfig config; config.model = "history-fixture";
        config.endpoint = QUrl("https://example.test/not-to-persist");
        config.credentialReference = "not-to-persist";
        const auto firstError = AgentHistory::record(config, 29, RunStatus::AwaitingReview, "reviewed-proposal");
        QVERIFY2(!firstError, firstError ? qPrintable(firstError->message) : "");
        const auto secondError = AgentHistory::record(config, 29, RunStatus::Completed, "reviewed-proposal", "applied-transaction");
        QVERIFY2(!secondError, secondError ? qPrintable(secondError->message) : "");
        const auto directory = historyBase_ + "/agent-history";
        const auto path = directory + "/history.json";
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll(); file.close();
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(bytes, &parseError);
        QCOMPARE(parseError.error, QJsonParseError::NoError); QVERIFY(document.isArray());
        const auto entries = document.array(); QCOMPARE(entries.size(), 2);
        QCOMPARE(entries[0].toObject().value("status").toString(), statusName(RunStatus::AwaitingReview));
        const auto applied = entries[1].toObject();
        QCOMPARE(applied.value("runId").toString(), QString("29"));
        QCOMPARE(applied.value("status").toString(), statusName(RunStatus::Completed));
        QCOMPARE(applied.value("proposalId").toString(), QString("reviewed-proposal"));
        QCOMPARE(applied.value("transactionId").toString(), QString("applied-transaction"));
        QVERIFY(!bytes.contains("not-to-persist"));
        for (const auto &entry : entries) QCOMPARE(entry.toObject().size(), 7);
#ifdef Q_OS_WIN
        QVERIFY(privateWindowsAcl(directory, true));
        QVERIFY(privateWindowsAcl(path, false));
#else
        const auto publicPermissions = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
            | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
        QVERIFY(!(QFile::permissions(directory) & publicPermissions));
        QVERIFY(!(QFile::permissions(path) & publicPermissions));
#endif
    }
    void historyFailureCannotReplaceConsumerVisibleRunFailure() {
        QVERIFY(QDir().mkpath(historyBase_));
        QFile blocker(historyBase_ + "/agent-history"); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.close();
        QTemporaryDir dir; desktop::WorkspaceController controller;
        controller.create(dir.filePath("model.mdl"), {}, AccessPolicy{{dir.path()}, {}, "ASCII", true}); QTRY_VERIFY(controller.hasWorkspace());
        RunPeer peer; AgentRun run(&controller); QSignalSpy events(&run, &AgentRun::event);
        const auto historyError = AgentHistory::record(peer.config(), 1, RunStatus::Failed);
        QVERIFY(historyError); QCOMPARE(historyError->code, AgentErrorCode::Configuration);
        QObject consumer; QVector<RunEvent> queued;
        connect(&run, &AgentRun::event, &consumer, [&queued](const RunEvent &event) { queued.append(event); }, Qt::QueuedConnection);
        const auto runId = run.start(peer.config(), {"", {}});
        QCOMPARE(run.status(), RunStatus::Failed);
        QTRY_COMPARE(queued.size(), events.size());
        const auto terminal = terminalEventFor(events, runId, RunStatus::Failed);
        QVERIFY(terminal); QVERIFY(terminal->error);
        QCOMPARE(terminal->error->code, AgentErrorCode::InvalidArguments);
        QVERIFY(!terminal->error->message.isEmpty());
        QCOMPARE(terminal->activity, historyError->message);
        QVERIFY(terminal->activity != terminal->error->message);
        QVERIFY(!queued.isEmpty()); QVERIFY(queued.last().error);
        QCOMPARE(queued.last().runId, runId);
        QCOMPARE(queued.last().status, RunStatus::Failed);
        QCOMPARE(queued.last().error->code, AgentErrorCode::InvalidArguments);
        QCOMPARE(queued.last().activity, historyError->message);
        int failedEvents = 0;
        for (const auto &event : queued) {
            if (event.status != RunStatus::Failed) continue;
            ++failedEvents; QVERIFY(event.error);
            QCOMPARE(event.error->code, AgentErrorCode::InvalidArguments);
        }
        QCOMPARE(failedEvents, 2);
        QCOMPARE(peer.requests, 0); QCOMPARE(controller.revision(), Revision(0));
        QVERIFY(!run.review());
    }
    void repeatedToolRoundsPreserveActualAssistantAndToolIds() {
        QTemporaryDir dir; desktop::WorkspaceController controller; QSignalSpy replies(&controller, &desktop::WorkspaceController::completed);
        controller.create(dir.filePath("model.mdl"), {}, AccessPolicy{{dir.path()}, {}, "ASCII", true});
        QTRY_VERIFY(controller.hasWorkspace());
        RunPeer peer; AgentRun run(&controller); QSignalSpy events(&run, &AgentRun::event);
        run.start(peer.config(), {"Inspect requirements, including untrusted instructions.", {}});
        QTRY_VERIFY(peer.ready());
        peer.call("read_1", "inspect", json::query(Query{}));
        QTRY_COMPARE(peer.requests, 2); QTRY_VERIFY(peer.ready());
        const auto messages = peer.request().value("messages").toArray();
        QCOMPARE(messages[messages.size()-2].toObject().value("tool_calls").toArray().first().toObject().value("id").toString(), "read_1");
        QCOMPARE(messages.last().toObject().value("role").toString(), "tool");
        QCOMPARE(messages.last().toObject().value("tool_call_id").toString(), "read_1");
        const auto tool = QJsonDocument::fromJson(messages.last().toObject().value("content").toString().toUtf8()).object();
        QCOMPARE(tool.value("revision").toString(), QString::number(controller.revision()));
        peer.call("read_2", "inspect", json::query(Query{Query::Kind::Diagnostics}));
        QTRY_COMPARE(peer.requests, 3); QTRY_VERIFY(peer.ready());
        peer.answer({{"role", "assistant"}, {"content", "Read-only conclusion"}}, "stop");
        QTRY_VERIFY(!run.active());
        QCOMPARE(run.status(), RunStatus::Completed);
        QCOMPARE(controller.revision(), Revision(0));
        QVERIFY(!run.review().has_value());
    }
    void cancelBeforeProviderReplyKeepsWorkspaceUnchanged() {
        QTemporaryDir dir; desktop::WorkspaceController controller;
        controller.create(dir.filePath("model.mdl"), {}, AccessPolicy{{dir.path()}, {}, "ASCII", true}); QTRY_VERIFY(controller.hasWorkspace());
        RunPeer peer; AgentRun run(&controller); run.start(peer.config(), {"Change the model", {}});
        QTRY_VERIFY(peer.ready()); const auto revision = controller.revision();
        run.cancel(); QTest::qWait(30);
        QCOMPARE(run.status(), RunStatus::Cancelled); QCOMPARE(controller.revision(), revision); QVERIFY(!run.review());
    }
    void manualApplyDuringRequestInvalidatesRun() {
        QTemporaryDir dir; desktop::WorkspaceController controller; QSignalSpy replies(&controller, &desktop::WorkspaceController::completed);
        const auto created = controller.create(dir.filePath("model.mdl"), {}, AccessPolicy{{dir.path()}, {}, "ASCII", true});
        QTRY_VERIFY(replyFor(replies, created));
        const auto initial = std::get<Projection>(*replyFor(replies, created));
        const auto owner = logicalOwner(initial); QVERIFY(!owner.isEmpty());
        RunPeer peer; AgentRun run(&controller); QSignalSpy events(&run, &AgentRun::event);
        const auto runId = run.start(peer.config(), {"Change the model", {}}); QTRY_VERIFY(peer.ready());
        const auto proposed = controller.propose(controller.revision(), {Command{CreateElement{"manual", "Class", "ManualWork", owner, {}}}});
        QTRY_VERIFY(replyFor(replies, proposed));
        const auto result = *replyFor(replies, proposed);
        QVERIFY(std::holds_alternative<Proposal>(result));
        const auto proposal = std::get_if<Proposal>(&result);
        QVERIFY(proposal); controller.apply({proposal->id, proposal->baseRevision, proposal->digest});
        QTRY_COMPARE(controller.revision(), Revision(1));
        QTRY_COMPARE(run.status(), RunStatus::Failed);
        const auto terminal = terminalEventFor(events, runId, RunStatus::Failed);
        QVERIFY(terminal); QVERIFY(terminal->error);
        QCOMPARE(terminal->error->code, AgentErrorCode::StaleRevision);
        QCOMPARE(peer.requests, 1);
        QVERIFY(!run.review());
        QCOMPARE(controller.revision(), Revision(1));
    }
    void destroyedReceiverDoesNotCommitLateProposal() {
        QTemporaryDir dir; desktop::WorkspaceController controller;
        controller.create(dir.filePath("model.mdl"), {}, AccessPolicy{{dir.path()}, {}, "ASCII", true}); QTRY_VERIFY(controller.hasWorkspace());
        RunPeer peer; auto *run = new AgentRun(&controller); run->start(peer.config(), {"Make a proposal", {}}); QTRY_VERIFY(peer.ready());
        delete run; QTest::qWait(30); QCOMPARE(controller.revision(), Revision(0));
    }
    void proposalRequiresExplicitApprovalAndNeverSaves() {
        QTemporaryDir dir; desktop::WorkspaceController controller;
        QSignalSpy replies(&controller, &desktop::WorkspaceController::completed);
        const auto path = dir.filePath("model.mdl");
        const auto created = controller.create(path, {}, AccessPolicy{{dir.path()}, {}, "ASCII", true});
        QTRY_VERIFY(replyFor(replies, created));
        const auto initial = std::get<Projection>(*replyFor(replies, created));
        const auto owner = logicalOwner(initial); QVERIFY(!owner.isEmpty());
        const auto baselineSave = controller.save();
        QTRY_VERIFY(replyFor(replies, baselineSave));
        QVERIFY(std::holds_alternative<SaveReceipt>(*replyFor(replies, baselineSave)));
        QFile before(path); QVERIFY(before.open(QIODevice::ReadOnly)); const auto disk = before.readAll(); before.close();
        RunPeer peer; AgentRun run(&controller);
        QSignalSpy events(&run, &AgentRun::event);
        run.start(peer.config(), {"Add a class to the selected logical package", {}}, {owner});
        QTRY_VERIFY(peer.ready());
        peer.call("propose_1", "submit_proposal", {{"baseRevision", "0"},
            {"commands", json::commands({Command{CreateElement{"reviewed", "Class", "ReviewedClass", owner, {}}}})}});
        QTRY_COMPARE(run.status(), RunStatus::AwaitingReview);
        QVERIFY(run.review()); QVERIFY(run.canApply()); QCOMPARE(controller.revision(), Revision(0));
        const auto displayed = *run.review();
        const Approval approval{displayed.id, displayed.baseRevision, displayed.digest};
        auto wrong = approval; wrong.digest = "different";
        QVERIFY(!run.applyReview(wrong));
        wrong = approval; ++wrong.baseRevision;
        QVERIFY(!run.applyReview(wrong));
        wrong = approval; wrong.id = ProposalId{"not-displayed"};
        QVERIFY(!run.applyReview(wrong));
        QVERIFY(!run.rejectReview(ProposalId{"not-displayed"}));
        QCOMPARE(run.status(), RunStatus::AwaitingReview); QVERIFY(run.canApply());
        QVERIFY(run.applyReview(approval));
        run.cancel(); QCOMPARE(run.status(), RunStatus::Applying);
        const auto applyingRun = run.id();
        QCOMPARE(run.start(peer.config(), {"Another request", {}}), applyingRun);
        QCOMPARE(run.status(), RunStatus::Applying);
        QTRY_COMPARE(run.status(), RunStatus::Completed); QCOMPARE(controller.revision(), Revision(1));
        QVERIFY(before.open(QIODevice::ReadOnly)); QCOMPARE(before.readAll(), disk);
        bool applied = false;
        for (const auto &reply : replies) applied |= std::holds_alternative<Applied>(qvariant_cast<WorkspaceReply>(reply[1]));
        QVERIFY(applied); QVERIFY(!displayed.digest.isEmpty()); QVERIFY(!run.review());
        QString transaction;
        for (const auto &reply : replies) {
            const auto result = qvariant_cast<WorkspaceReply>(reply[1]);
            if (auto value = std::get_if<Applied>(&result)) transaction = value->transactionId;
        }
        QVERIFY(!transaction.isEmpty());
        bool correlated = false;
        for (const auto &entry : events) {
            const auto value = qvariant_cast<RunEvent>(entry[0]);
            if (value.status == RunStatus::Completed && value.transactionId == transaction && value.runId == applyingRun) correlated = true;
            if (value.status == RunStatus::AwaitingReview || value.status == RunStatus::Applying) QVERIFY(value.transactionId.isEmpty());
        }
        QVERIFY(correlated);
        before.close();
        const auto saved = controller.save();
        QTRY_VERIFY(replyFor(replies, saved));
        QVERIFY(std::holds_alternative<SaveReceipt>(*replyFor(replies, saved)));
        QVERIFY(before.open(QIODevice::ReadOnly)); QVERIFY(before.readAll() != disk); before.close();
        auto reopened = Workspace::open(path, AccessPolicy{{dir.path()}, {}, "ASCII", true});
        QVERIFY(std::holds_alternative<std::unique_ptr<Workspace>>(reopened));
        const auto persisted = std::get<Projection>(std::get<std::unique_ptr<Workspace>>(reopened)->inspect({}));
        bool found = false;
        for (const auto &element : persisted.elements)
            found |= element.name == "ReviewedClass" && element.id.value == displayed.newIds.value("reviewed");
        QVERIFY(found);
    }
    void staleReviewCannotApproveManualChanges() {
        QTemporaryDir dir; desktop::WorkspaceController controller;
        QSignalSpy replies(&controller, &desktop::WorkspaceController::completed);
        const auto created = controller.create(dir.filePath("model.mdl"), {}, AccessPolicy{{dir.path()}, {}, "ASCII", true});
        QTRY_VERIFY(replyFor(replies, created));
        const auto initial = std::get<Projection>(*replyFor(replies, created));
        const auto owner = logicalOwner(initial); QVERIFY(!owner.isEmpty());
        RunPeer peer; AgentRun run(&controller); run.start(peer.config(), {"Add a class", {}});
        QTRY_VERIFY(peer.ready());
        peer.call("proposal", "submit_proposal", {{"baseRevision", "0"},
            {"commands", json::commands({Command{CreateElement{"agent", "Class", "AgentClass", owner, {}}}})}});
        QTRY_COMPARE(run.status(), RunStatus::AwaitingReview);
        const Approval displayed{run.review()->id, run.review()->baseRevision, run.review()->digest};
        const auto request = controller.propose(0, {Command{CreateElement{"manual", "Class", "ManualClass", owner, {}}}});
        QTRY_VERIFY(replyFor(replies, request));
        std::optional<Proposal> manual;
        for (const auto &reply : replies) if (reply[0].toULongLong() == request) {
            const auto value = qvariant_cast<WorkspaceReply>(reply[1]);
            if (auto proposal = std::get_if<Proposal>(&value)) manual = *proposal;
        }
        QVERIFY(manual); controller.apply({manual->id, manual->baseRevision, manual->digest});
        QTRY_COMPARE(controller.revision(), Revision(1)); QTRY_COMPARE(run.status(), RunStatus::Failed);
        QVERIFY(!run.canApply()); QVERIFY(!run.applyReview(displayed)); QTest::qWait(20); QCOMPARE(controller.revision(), Revision(1));
    }
    void rejectedProposalDoesNotAdvanceRevision() {
        QTemporaryDir dir; desktop::WorkspaceController controller;
        QSignalSpy replies(&controller, &desktop::WorkspaceController::completed);
        const auto created = controller.create(dir.filePath("model.mdl"), {}, AccessPolicy{{dir.path()}, {}, "ASCII", true});
        QTRY_VERIFY(replyFor(replies, created));
        const auto initial = std::get<Projection>(*replyFor(replies, created));
        const auto owner = logicalOwner(initial); QVERIFY(!owner.isEmpty());
        RunPeer peer; AgentRun run(&controller); run.start(peer.config(), {"Add a class", {}}); QTRY_VERIFY(peer.ready());
        peer.call("proposal", "submit_proposal", {{"baseRevision", "0"},
            {"commands", json::commands({Command{CreateElement{"rejected", "Class", "RejectedClass", owner, {}}}})}});
        QTRY_COMPARE(run.status(), RunStatus::AwaitingReview); QVERIFY(run.rejectReview(run.review()->id));
        QTRY_COMPARE(run.status(), RunStatus::Completed); QCOMPARE(controller.revision(), Revision(0)); QVERIFY(!run.review());
    }
    void decodedImagesDoNotTrustExtensions_data() {
        QTest::addColumn<QByteArray>("format");
        QTest::addColumn<QString>("mime");
        QTest::newRow("png") << QByteArray("PNG") << QString("image/png");
        QTest::newRow("jpeg") << QByteArray("JPEG") << QString("image/jpeg");
        if (QImageReader::supportedImageFormats().contains("webp") && QImageWriter::supportedImageFormats().contains("webp"))
            QTest::newRow("webp") << QByteArray("WEBP") << QString("image/webp");
    }
    void decodedImagesDoNotTrustExtensions() {
        QFETCH(QByteArray, format); QFETCH(QString, mime);
        QTemporaryDir dir;
        const auto bytes = imageBytes(format.constData()); QVERIFY(!bytes.isEmpty());
        QFile file(dir.filePath("wrong-extension.txt")); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size()); file.close();
        const auto loaded = loadImage(file.fileName());
        QVERIFY(std::holds_alternative<AgentImage>(loaded));
        QCOMPARE(std::get<AgentImage>(loaded).bytes, bytes);
        QCOMPARE(std::get<AgentImage>(loaded).mimeType, mime);
        QVERIFY(!validateInput({"", {std::get<AgentImage>(loaded)}}, 1024 * 1024));
        auto incorrect = std::get<AgentImage>(loaded); incorrect.mimeType = "image/svg+xml";
        const auto mimeError = validateInput({"", {incorrect}}, 1024 * 1024);
        QVERIFY(mimeError);
        QCOMPARE(mimeError->code, AgentErrorCode::InvalidArguments);
    }
    void invalidAndOverBudgetImagesNeverReachProvider() {
        QTemporaryDir dir; desktop::WorkspaceController controller;
        controller.create(dir.filePath("model.mdl"), {}, AccessPolicy{{dir.path()}, {}, "ASCII", true}); QTRY_VERIFY(controller.hasWorkspace());
        RunPeer peer; AgentRun run(&controller); QSignalSpy events(&run, &AgentRun::event);
        const auto invalidRun = run.start(peer.config(), {"Read", {{"image/png", "<html>not an image</html>"}}});
        QCOMPARE(run.status(), RunStatus::Failed); QCOMPARE(peer.requests, 0);
        const auto invalidTerminal = terminalEventFor(events, invalidRun, RunStatus::Failed);
        QVERIFY(invalidTerminal); QVERIFY(invalidTerminal->error);
        QCOMPARE(invalidTerminal->error->code, AgentErrorCode::InvalidArguments);
        const auto bytes = imageBytes("PNG", 128); QVERIFY(!bytes.isEmpty());
        QVERIFY(4 * ((bytes.size() + 2) / 3) > 1024);
        auto config = peer.config(); config.maxContextBytes = 1024;
        const auto overBudgetRun = run.start(config, {"", {{"image/png", bytes}}});
        QCOMPARE(run.status(), RunStatus::Failed); QCOMPARE(peer.requests, 0);
        const auto overBudgetTerminal = terminalEventFor(events, overBudgetRun, RunStatus::Failed);
        QVERIFY(overBudgetTerminal); QVERIFY(overBudgetTerminal->error);
        QCOMPARE(overBudgetTerminal->error->code, AgentErrorCode::ContextBudgetExceeded);
        QVERIFY(validateInput({"", {{"image/png", bytes.left(bytes.size() / 2)}}}, 1024 * 1024));
        QVERIFY(validateInput({"", QVector<AgentImage>(5, AgentImage{"image/png", imageBytes("PNG")})}, 1024 * 1024));
        QVERIFY(validateInput({"", {{"image/png", QByteArray(8 * 1024 * 1024 + 1, 'x')}}}, 64 * 1024 * 1024));
        QImage wide(8193, 1, QImage::Format_RGB32); wide.fill(Qt::white);
        QByteArray oversizedDimensions; QBuffer output(&oversizedDimensions); QVERIFY(output.open(QIODevice::WriteOnly));
        QVERIFY(wide.save(&output, "PNG")); output.close();
        const auto dimensionError = validateInput({"", {{"image/png", oversizedDimensions}}}, 1024 * 1024);
        QVERIFY(dimensionError);
        QCOMPARE(dimensionError->code, AgentErrorCode::InvalidArguments);
        QFile fake(dir.filePath("renamed.png")); QVERIFY(fake.open(QIODevice::WriteOnly));
        QVERIFY(fake.write("<html>not an image</html>") > 0); fake.close();
        QVERIFY(std::holds_alternative<AgentError>(loadImage(fake.fileName())));
        QVERIFY(std::holds_alternative<AgentError>(loadImage(dir.filePath("missing.png"))));
        QTest::qWait(20); QCOMPARE(peer.requests, 0);
        QCOMPARE(controller.revision(), Revision(0)); QVERIFY(!run.review());
    }
    void oldDisplayedTupleCannotApproveOrRejectReplacementReview() {
        QTemporaryDir dir; desktop::WorkspaceController controller; QSignalSpy replies(&controller, &desktop::WorkspaceController::completed);
        const auto created = controller.create(dir.filePath("model.mdl"), {}, AccessPolicy{{dir.path()}, {}, "ASCII", true});
        QTRY_VERIFY(replyFor(replies, created));
        const auto initial = std::get<Projection>(*replyFor(replies, created));
        const auto owner = logicalOwner(initial); QVERIFY(!owner.isEmpty());
        RunPeer peer; AgentRun run(&controller); run.start(peer.config(), {"First edit", {}}); QTRY_VERIFY(peer.ready());
        peer.call("first", "submit_proposal", {{"baseRevision", "0"}, {"commands", json::commands({Command{CreateElement{"first", "Class", "First", owner, {}}}})}});
        QTRY_COMPARE(run.status(), RunStatus::AwaitingReview);
        const Approval old{run.review()->id, run.review()->baseRevision, run.review()->digest};
        run.start(peer.config(), {"Replacement edit", {}}); QTRY_COMPARE(peer.requests, 2); QTRY_VERIFY(peer.ready());
        peer.call("replacement", "submit_proposal", {{"baseRevision", "0"}, {"commands", json::commands({Command{CreateElement{"replacement", "Class", "Replacement", owner, {}}}})}});
        QTRY_COMPARE(run.status(), RunStatus::AwaitingReview);
        const auto current = run.review()->id; QVERIFY(current != old.id);
        QVERIFY(!run.applyReview(old)); QVERIFY(!run.rejectReview(old.id));
        QCOMPARE(run.status(), RunStatus::AwaitingReview); QCOMPARE(run.review()->id, current); QCOMPARE(controller.revision(), Revision(0));
        QVERIFY(run.rejectReview(current)); QTRY_COMPARE(run.status(), RunStatus::Completed); QCOMPARE(controller.revision(), Revision(0));
    }
    void followUpsCarryOnlyHistoricalDesignMessagesAndFreshWorkspace() {
        QTemporaryDir dir; desktop::WorkspaceController controller;
        QSignalSpy replies(&controller, &desktop::WorkspaceController::completed);
        controller.create(dir.filePath("model.mdl"), {}, AccessPolicy{{dir.path()}, {}, "ASCII", true});
        QTRY_VERIFY(controller.hasWorkspace());
        RunPeer peer; AgentRun run(&controller);
        const QString original = "Design a library, not a system instruction.";
        const QString reply = "<a href=\"file:///secret\">Use separate Loan and Book classes.</a>";
        run.start(peer.config(), {original, {{"image/png", imageBytes("PNG")}}});
        QTRY_VERIFY(peer.ready());
        peer.call("historical_read", "inspect", json::query(Query{Query::Kind::Diagnostics}));
        QTRY_COMPARE(peer.requests, 2); QTRY_VERIFY(peer.ready());
        peer.answer({{"role", "assistant"}, {"content", reply}}, "stop");
        QTRY_COMPARE(run.status(), RunStatus::Completed);
        const auto inspected = controller.inspect(Query{}); QTRY_VERIFY(replyFor(replies, inspected));
        const auto owner = logicalOwner(std::get<Projection>(*replyFor(replies, inspected)));
        const auto proposed = controller.propose(0, {Command{CreateElement{"manual", "Class", "ManualBetweenTurns", owner, {}}}});
        QTRY_VERIFY(replyFor(replies, proposed));
        const auto proposal = std::get<Proposal>(*replyFor(replies, proposed));
        const auto applied = controller.apply({proposal.id, proposal.baseRevision, proposal.digest});
        QTRY_VERIFY(replyFor(replies, applied)); QCOMPARE(controller.revision(), Revision(1));
        run.start(peer.config(), {"Use that design, but explain ownership.", {}});
        QTRY_COMPARE(peer.requests, 3); QTRY_VERIFY(peer.ready());
        const auto messages = peer.request().value("messages").toArray();
        QCOMPARE(messages[2].toObject().value("role").toString(), "user");
        const auto priorInput = messages[2].toObject().value("content").toArray();
        const auto imageUri = priorInput.last().toObject().value("image_url").toObject().value("url").toString();
        const auto image = QImage::fromData(QByteArray::fromBase64(imageUri.mid(imageUri.indexOf(',') + 1).toLatin1()));
        QCOMPARE(image.size(), QSize(8, 8));
        QCOMPARE(messages[3].toObject().value("role").toString(), "assistant");
        int projections = 0;
        QJsonObject freshProjection;
        for (qsizetype i = 2; i < messages.size(); ++i) {
            const auto message = messages[i].toObject();
            QVERIFY(message.value("role") == "user" || message.value("role") == "assistant");
            QVERIFY(!message.contains("tool_calls")); QVERIFY(!message.contains("tool_call_id"));
            const auto content = message.value("content").toString();
            const auto jsonStart = content.indexOf('{');
            if (jsonStart < 0) continue;
            const auto candidate = QJsonDocument::fromJson(content.mid(jsonStart).toUtf8()).object();
            if (candidate.value("type") == "projection") { ++projections; freshProjection = candidate; }
        }
        QCOMPARE(projections, 1);
        QCOMPARE(freshProjection.value("revision").toString(), "1");
        const auto createdId = std::get<Applied>(*replyFor(replies, applied)).newIds.value("manual");
        bool foundCreated = false;
        for (const auto &entry : freshProjection.value("elements").toArray())
            if (entry.toObject().value("id") == createdId) foundCreated = true;
        QVERIFY(foundCreated);
        peer.answer({{"role", "assistant"}, {"content", "Ownership explanation"}}, "stop");
        QTRY_COMPARE(run.status(), RunStatus::Completed);
    }
    void workspaceGenerationClearsConversationContext() {
        QTemporaryDir dir; desktop::WorkspaceController controller; QSignalSpy replies(&controller, &desktop::WorkspaceController::completed);
        const auto first = controller.create(dir.filePath("first.mdl"), {}, AccessPolicy{{dir.path()}, {}, "ASCII", true});
        QTRY_VERIFY(replyFor(replies, first));
        RunPeer peer; AgentRun run(&controller);
        run.start(peer.config(), {"Old workspace design", {}}); QTRY_VERIFY(peer.ready());
        peer.answer({{"role", "assistant"}, {"content", "Old workspace explanation"}}, "stop");
        QTRY_COMPARE(run.status(), RunStatus::Completed);
        const auto next = controller.create(dir.filePath("next.mdl"), {}, AccessPolicy{{dir.path()}, {}, "ASCII", true});
        QTRY_VERIFY(replyFor(replies, next));
        run.start(peer.config(), {"New workspace design", {}});
        QTRY_COMPARE(peer.requests, 2); QTRY_VERIFY(peer.ready());
        const auto bytes = QJsonDocument(peer.request().value("messages").toArray()).toJson(QJsonDocument::Compact);
        QVERIFY(!bytes.contains("Old workspace design")); QVERIFY(!bytes.contains("Old workspace explanation"));
        QVERIFY(bytes.contains("New workspace design"));
        run.cancel();
    }
    void conversationBudgetFailsRatherThanSilentlyDroppingHistory() {
        QTemporaryDir dir; desktop::WorkspaceController controller;
        controller.create(dir.filePath("model.mdl"), {}, AccessPolicy{{dir.path()}, {}, "ASCII", true}); QTRY_VERIFY(controller.hasWorkspace());
        RunPeer peer; AgentRun run(&controller); QSignalSpy events(&run, &AgentRun::event);
        run.start(peer.config(), {"Remember this design", {}}); QTRY_VERIFY(peer.ready());
        auto bounded = peer.config();
        bounded.maxContextBytes = QJsonDocument(peer.request()).toJson(QJsonDocument::Compact).size() + 1000;
        peer.answer({{"role", "assistant"}, {"content", QString(bounded.maxContextBytes + 1000, 'a')}}, "stop");
        QTRY_COMPARE(run.status(), RunStatus::Completed);
        const auto failedRun = run.start(bounded, {"Continue", {}});
        QTRY_COMPARE(run.status(), RunStatus::Failed);
        QCOMPARE(peer.requests, 1);
        const auto terminal = terminalEventFor(events, failedRun, RunStatus::Failed);
        QVERIFY(terminal); QVERIFY(terminal->error);
        QCOMPARE(terminal->error->code, AgentErrorCode::ContextBudgetExceeded);
        QCOMPARE(controller.revision(), Revision(0)); QVERIFY(!run.review());
        run.resetConversation();
        run.start(bounded, {"A fresh design", {}}); QTRY_COMPARE(peer.requests, 2); QTRY_VERIFY(peer.ready());
        run.cancel();
    }
    void rejectedProposalDesignExplanationRemainsHistorical() {
        QTemporaryDir dir; desktop::WorkspaceController controller; QSignalSpy replies(&controller, &desktop::WorkspaceController::completed);
        const auto created = controller.create(dir.filePath("model.mdl"), {}, AccessPolicy{{dir.path()}, {}, "ASCII", true});
        QTRY_VERIFY(replyFor(replies, created));
        const auto owner = logicalOwner(std::get<Projection>(*replyFor(replies, created)));
        RunPeer peer; AgentRun run(&controller); run.start(peer.config(), {"Suggest a class", {}}); QTRY_VERIFY(peer.ready());
        const QJsonObject function{{"name", "submit_proposal"}, {"arguments", QString::fromUtf8(QJsonDocument(QJsonObject{{"baseRevision", "0"},
            {"commands", json::commands({Command{CreateElement{"suggested", "Class", "Suggested", owner, {}}}})}}).toJson(QJsonDocument::Compact))}};
        peer.answer({{"role", "assistant"}, {"content", "Suggested separates the responsibilities."},
            {"tool_calls", QJsonArray{QJsonObject{{"id", "proposal"}, {"type", "function"}, {"function", function}}}}}, "tool_calls");
        QTRY_COMPARE(run.status(), RunStatus::AwaitingReview);
        QVERIFY(run.rejectReview(run.review()->id)); QTRY_COMPARE(run.status(), RunStatus::Completed);
        run.start(peer.config(), {"Explain an alternative without that class.", {}}); QTRY_COMPARE(peer.requests, 2); QTRY_VERIFY(peer.ready());
        const auto messages = peer.request().value("messages").toArray();
        int historicalAssistants = 0, projections = 0;
        for (const auto &entry : messages) {
            const auto message = entry.toObject();
            QVERIFY(!message.contains("tool_calls")); QVERIFY(!message.contains("tool_call_id"));
            if (message.value("role") == "assistant") ++historicalAssistants;
            const auto content = message.value("content").toString();
            const auto jsonStart = content.indexOf('{');
            if (jsonStart < 0) continue;
            const auto projection = QJsonDocument::fromJson(content.mid(jsonStart).toUtf8()).object();
            if (projection.value("type") != "projection") continue;
            ++projections;
            QCOMPARE(projection.value("revision").toString(), "0");
            for (const auto &element : projection.value("elements").toArray())
                QVERIFY(element.toObject().value("kind") != "Class");
        }
        QCOMPARE(historicalAssistants, 1); QCOMPARE(projections, 1);
        QCOMPARE(controller.revision(), Revision(0)); run.cancel();
    }
    void synchronousCancellationAtPlanningEventRemainsTerminal() {
        QTemporaryDir dir; desktop::WorkspaceController controller;
        controller.create(dir.filePath("model.mdl"), {}, AccessPolicy{{dir.path()}, {}, "ASCII", true}); QTRY_VERIFY(controller.hasWorkspace());
        RunPeer peer; AgentRun run(&controller);
        connect(&run, &AgentRun::event, &run, [&run](const RunEvent &event) {
            if (event.status == RunStatus::Planning) run.cancel();
        });
        run.start(peer.config(), {"Cancelled before context", {}});
        QCOMPARE(run.status(), RunStatus::Cancelled); QCOMPARE(peer.requests, 0); QCOMPARE(controller.revision(), Revision(0));
    }
};
QTEST_GUILESS_MAIN(AgentRunTests)
#include "tst_agent_runs.moc"
