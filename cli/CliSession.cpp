#include "CliSession.h"
#include "StdinReader.h"
#include "agent/ProviderSettings.h"
#include "core/Json.h"
#include "core/Storage.h"
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSettings>
#include <QTimer>
#include <limits>
#include <utility>

namespace rose::cli {
namespace {
WorkspaceError invalid(QString message) { return {ErrorCode::InvalidCommand, std::move(message), {}}; }
std::optional<WorkspaceError> fields(const QJsonObject &object, const QStringList &required, const QStringList &optional = {}) {
    for (const auto &key : required) if (!object.contains(key)) return invalid(key + " is required");
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!required.contains(it.key()) && !optional.contains(it.key())) return invalid("Unknown field: " + it.key());
    return {};
}
std::optional<WorkspaceError> fileField(const QJsonObject &object, const QString &key) {
    if (!object.value(key).isString() || object.value(key).toString().isEmpty()
        || object.value(key).toString().contains(QChar::Null)) return invalid(key + " must be a nonempty file path string");
    return {};
}
Outcome<ProposalId> proposalId(const QJsonValue &id) {
    auto parsed = json::approval({{"id", id}, {"baseRevision", "0"}, {"digest", QString(64, QLatin1Char('0'))}});
    if (auto error = std::get_if<WorkspaceError>(&parsed)) return *error;
    return std::get<Approval>(parsed).id;
}
QJsonObject errorObject(const agent::AgentError &error) {
    return {{"code", agent::errorName(error.code)}, {"message", error.message}};
}
bool terminal(agent::RunStatus status) {
    return status == agent::RunStatus::Completed || status == agent::RunStatus::Cancelled || status == agent::RunStatus::Failed;
}
}
CliSession::CliSession(AccessPolicy policy, QFile &output, QFile &errors, QObject *parent)
    : QObject(parent), policy_(std::move(policy)), output_(output), errors_(errors), controller_(this), run_(&controller_, this) {
    connect(&controller_, &desktop::WorkspaceController::completed, this, &CliSession::workspaceCompleted);
    connect(&run_, &agent::AgentRun::event, this, &CliSession::agentEvent);
}
CliSession::~CliSession() = default;
void CliSession::start(const QString &file) {
    if (file.isEmpty()) startReader();
    else startupRequest_ = controller_.open(file, policy_);
}
void CliSession::startReader() {
    input_ = std::make_unique<StdinReader>(this, [this](QByteArray bytes) { readLine(bytes); },
        [this](QString error) { endInput(error); });
}
bool CliSession::writeJson(QFile &output, const QJsonObject &object) {
    if (outputFailed_) return false;
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    if (output.write(bytes) == bytes.size() && output.flush()) return true;
    outputFailed_ = true; exitCode_ = 1; eof_ = true;
    requests_.clear();
    input_.reset();
    if (run_.status() != agent::RunStatus::Applying) run_.cancel();
    QTimer::singleShot(0, this, &CliSession::finishIfReady);
    return false;
}
void CliSession::reply(const QString &id, const QJsonObject &value, bool ok) {
    QJsonObject object{{"id", id.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(id)}, {"ok", ok},
        {ok ? "result" : "error", value}};
    if (controller_.hasWorkspace()) object.insert("revision", QString::number(controller_.revision()));
    writeJson(output_, object);
    inFlight_.remove(id);
}
void CliSession::reply(const QString &id, const WorkspaceReply &value) {
    reply(id, json::reply(value), !std::holds_alternative<WorkspaceError>(value));
}
void CliSession::agentError(const QString &id, const agent::AgentError &error) { reply(id, errorObject(error), false); }
void CliSession::readLine(const QByteArray &line) {
    if (eof_) return;
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        reply({}, WorkspaceError{ErrorCode::InvalidSyntax,
            parseError.error == QJsonParseError::NoError ? QStringLiteral("Request must be a JSON object") : parseError.errorString(),
            {}, parseError.offset, 1, parseError.offset + 1});
        return;
    }
    const auto object = document.object();
    const auto idValue = object.value("id");
    const auto id = idValue.isString() ? idValue.toString() : QString{};
    if (inFlight_.contains(id)) {
        // Do not release the original request's reservation when rejecting its duplicate.
        QJsonObject duplicate{{"id", id}, {"ok", false}, {"error", json::error(invalid("Request id is already in flight"))}};
        if (controller_.hasWorkspace()) duplicate.insert("revision", QString::number(controller_.revision()));
        writeJson(output_, duplicate);
        return;
    }
    if (auto error = fields(object, {"id", "method", "params"})) { reply(id, *error); return; }
    if (id.isEmpty()) { reply({}, invalid("Request id must be a nonempty string")); return; }
    if (!object.value("method").isString() || !object.value("params").isObject()) {
        reply(id, invalid("method must be a string and params must be an object")); return;
    }
    inFlight_.insert(id);
    requests_.enqueue({id, object.value("method").toString(), object.value("params").toObject()});
    drain();
}
void CliSession::endInput(const QString &error) {
    eof_ = true;
    if (!error.isEmpty()) { exitCode_ = 1; reply({}, WorkspaceError{ErrorCode::StorageFailure, error, {}}); }
    drain();
}
void CliSession::drain() {
    if (pending_ || finishing_) return;
    if (requests_.isEmpty()) { finishIfReady(); return; }
    const auto &next = requests_.head();
    // Session-changing requests must not supersede an approved atomic commit.
    if (run_.status() == agent::RunStatus::Applying
        && (next.method == "close" || next.method == "open" || next.method == "create" || next.method == "access")) return;
    const auto request = requests_.dequeue();
    dispatch(request);
    if (!pending_) QTimer::singleShot(0, this, &CliSession::drain);
}
void CliSession::dispatch(const Request &request) {
    const auto &method = request.method;
    const auto &params = request.params;
    auto fail = [&](const WorkspaceError &error) { reply(request.id, error); };
    if (method.startsWith("agent.")) { dispatchAgent(request); return; }
    quint64 queued = 0;
    if (method == "open" || method == "create") {
        if (auto error = fields(params, {"file"}, method == "create" ? QStringList{"petalVersion", "encoding"} : QStringList{})) { fail(*error); return; }
        if (auto error = fileField(params, "file")) { fail(*error); return; }
        if (controller_.hasWorkspace()) { fail(invalid("Close the existing workspace before opening or creating another")); return; }
        if (method == "open") queued = controller_.open(params.value("file").toString(), policy_);
        else {
            RoseProfile profile; profile.encoding = policy_.sourceEncoding;
            if (params.contains("petalVersion")) {
                const auto version = params.value("petalVersion");
                if (!version.isDouble() || version.toDouble() != version.toInt() || version.toInt() <= 0) { fail(invalid("petalVersion must be a positive integer")); return; }
                profile.petalVersion = version.toInt();
            }
            if (params.contains("encoding")) {
                if (!params.value("encoding").isString() || params.value("encoding").toString().isEmpty()) { fail(invalid("encoding must be a nonempty string")); return; }
                profile.encoding = params.value("encoding").toString().toLatin1();
            }
            queued = controller_.create(params.value("file").toString(), profile, policy_);
        }
    } else if (!controller_.hasWorkspace()) { fail(invalid("No workspace is open")); return; }
    else if (method == "inspect") {
        auto parsed = json::query(params);
        if (auto error = std::get_if<WorkspaceError>(&parsed)) { fail(*error); return; }
        queued = controller_.inspect(std::get<Query>(parsed));
    } else if (method == "propose") {
        if (auto error = fields(params, {"baseRevision", "commands"})) { fail(*error); return; }
        if (!params.value("commands").isArray()) { fail(invalid("commands must be an array")); return; }
        auto revision = json::revision(params.value("baseRevision"));
        if (auto error = std::get_if<WorkspaceError>(&revision)) { fail(*error); return; }
        auto commands = json::commands(params.value("commands").toArray());
        if (auto error = std::get_if<WorkspaceError>(&commands)) { fail(*error); return; }
        queued = controller_.propose(std::get<Revision>(revision), std::get<QVector<Command>>(commands));
    } else if (method == "apply") {
        auto approval = json::approval(params);
        if (auto error = std::get_if<WorkspaceError>(&approval)) { fail(*error); return; }
        if (run_.review() && std::get<Approval>(approval).id == run_.review()->id) {
            fail(invalid("Use agent.apply with the runId and exact displayed approval for an agent proposal")); return;
        }
        queued = controller_.apply(std::get<Approval>(approval));
    } else if (method == "reject") {
        if (auto error = fields(params, {"id"})) { fail(*error); return; }
        auto parsed = proposalId(params.value("id"));
        if (auto error = std::get_if<WorkspaceError>(&parsed)) { fail(*error); return; }
        if (run_.review() && std::get<ProposalId>(parsed) == run_.review()->id) {
            fail(invalid("Use agent.reject with the runId for an agent proposal")); return;
        }
        queued = controller_.reject(std::get<ProposalId>(parsed));
    } else if (method == "undo" || method == "redo") {
        if (auto error = fields(params, {})) { fail(*error); return; }
        queued = method == "undo" ? controller_.undo() : controller_.redo();
    } else if (method == "save") {
        if (auto error = fields(params, {}, {"destination"})) { fail(*error); return; }
        if (params.contains("destination")) if (auto error = fileField(params, "destination")) { fail(*error); return; }
        queued = controller_.save({params.value("destination").toString()});
    } else if (method == "close") {
        if (auto error = fields(params, {}, {"discard"})) { fail(*error); return; }
        if (params.contains("discard") && !params.value("discard").isBool()) { fail(invalid("discard must be boolean")); return; }
        checkingClose_ = true;
        queued = controller_.inspect(Query{});
    } else if (method == "access") {
        if (auto error = fields(params, {"directory"})) { fail(*error); return; }
        if (auto error = fileField(params, "directory")) { fail(*error); return; }
        queued = controller_.grantDirectory(params.value("directory").toString());
    } else { fail(invalid("Unknown session method: " + method)); return; }
    pending_ = request;
    workspaceRequest_ = queued;
}
void CliSession::workspaceCompleted(quint64 request, const WorkspaceReply &value) {
    if (request == startupRequest_ && startupRequest_) {
        startupRequest_ = 0;
        if (auto error = std::get_if<WorkspaceError>(&value)) {
            writeJson(errors_, json::error(*error)); finishing_ = true; emit finished(1);
        } else startReader();
        return;
    }
    if (!pending_ || request != workspaceRequest_) return;
    if (checkingClose_) {
        checkingClose_ = false;
        if (auto projection = std::get_if<Projection>(&value)) {
            if (projection->dirty && !pending_->params.value("discard").toBool()) {
                reply(pending_->id, invalid("Workspace has unsaved changes; save or explicitly close with discard:true"));
            } else {
                run_.cancel();
                workspaceRequest_ = controller_.close();
                return;
            }
        } else reply(pending_->id, value);
    } else {
        if (std::holds_alternative<AccessGranted>(value)) {
            auto next = policy_;
            next.allowedDirectories.append(pending_->params.value("directory").toString());
            auto canonical = storage::canonicalPolicy(next);
            if (auto policy = std::get_if<AccessPolicy>(&canonical)) policy_ = *policy;
        }
        reply(pending_->id, value);
    }
    pending_.reset(); workspaceRequest_ = 0;
    drain();
}
void CliSession::dispatchAgent(const Request &request) {
    using namespace agent;
    const auto &params = request.params;
    auto fail = [&](QString message) { reply(request.id, invalid(std::move(message))); };
    if (request.method == "agent.configure") {
        if (auto error = fields(params, {"kind", "endpoint", "model"}, {"credentialReference", "timeoutMs", "maxRounds", "maxToolCalls", "maxContextBytes", "maxResponseBytes"})) { reply(request.id, *error); return; }
        ProviderConfig config;
        if (params.value("kind") == "local") config.kind = ProviderKind::Local;
        else if (params.value("kind") == "hosted") config.kind = ProviderKind::Hosted;
        else { fail("kind must be local or hosted"); return; }
        for (const auto &field : {QStringLiteral("endpoint"), QStringLiteral("model"), QStringLiteral("credentialReference")}) {
            if (params.contains(field) && !params.value(field).isString()) { fail(field + " must be a string"); return; }
        }
        config.endpoint = QUrl(params.value("endpoint").toString(), QUrl::StrictMode);
        config.model = params.value("model").toString();
        config.credentialReference = params.value("credentialReference").toString();
        const std::pair<const char *, int *> budgets[] = {
            {"timeoutMs", &config.timeoutMs}, {"maxRounds", &config.maxRounds}, {"maxToolCalls", &config.maxToolCalls},
            {"maxContextBytes", &config.maxContextBytes}, {"maxResponseBytes", &config.maxResponseBytes}};
        for (const auto &[name, target] : budgets) {
            if (!params.contains(QLatin1String(name))) continue;
            const auto value = params.value(QLatin1String(name));
            if (!value.isDouble() || value.toDouble() != value.toInt() || value.toInt() <= 0) { fail(QString::fromLatin1(name) + " must be a positive integer"); return; }
            *target = value.toInt();
        }
        QSettings settings(QSettings::NativeFormat, QSettings::UserScope, "RoseAgent", "RoseAgent");
        if (auto error = ProviderSettings::store(settings, config)) { agentError(request.id, *error); return; }
        reply(request.id, QJsonObject{{"configured", true}});
    } else if (request.method == "agent.start") {
        if (auto error = fields(params, {"prompt"}, {"images", "selectedIds"})) { reply(request.id, *error); return; }
        if (!params.value("prompt").isString()) { fail("prompt must be a string"); return; }
        if (run_.active() || run_.review() || !agentReviewRequest_.isEmpty()) { fail("Finish, reject, or cancel the current agent run before starting another"); return; }
        AgentInput input; input.prompt = params.value("prompt").toString();
        if (params.contains("images")) {
            if (!params.value("images").isArray()) { fail("images must be an array of file paths"); return; }
            for (const auto &image : params.value("images").toArray()) {
                if (!image.isString() || image.toString().isEmpty() || image.toString().contains(QChar::Null)) { fail("images must contain nonempty file path strings"); return; }
                auto path = storage::canonicalPath(image.toString(), policy_);
                if (auto error = std::get_if<WorkspaceError>(&path)) { reply(request.id, *error); return; }
                auto loaded = loadImage(std::get<QString>(path));
                if (auto error = std::get_if<AgentError>(&loaded)) { agentError(request.id, *error); return; }
                input.images.append(std::get<AgentImage>(std::move(loaded)));
            }
        }
        QVector<ElementId> selected;
        if (params.contains("selectedIds")) {
            if (!params.value("selectedIds").isArray()) { fail("selectedIds must be an array of element IDs"); return; }
            if (!params.value("selectedIds").toArray().isEmpty()) {
                auto parsed = json::query({{"kind", "elementsById"}, {"elements", params.value("selectedIds")}});
                if (auto error = std::get_if<WorkspaceError>(&parsed)) { reply(request.id, *error); return; }
                selected = std::get<Query>(parsed).elements;
            }
        }
        QSettings settings(QSettings::NativeFormat, QSettings::UserScope, "RoseAgent", "RoseAgent");
        auto config = ProviderSettings::load(settings);
        if (auto error = std::get_if<AgentError>(&config)) { agentError(request.id, *error); return; }
        const auto runId = run_.start(std::get<ProviderConfig>(config), input, selected);
        reply(request.id, QJsonObject{{"runId", QString::number(runId)}});
    } else if (request.method == "agent.cancel") {
        if (auto error = fields(params, {})) { reply(request.id, *error); return; }
        run_.cancel();
        reply(request.id, QJsonObject{{"runId", QString::number(run_.id())}, {"status", statusName(run_.status())}});
    } else if (request.method == "agent.apply" || request.method == "agent.reject") {
        const bool applying = request.method == "agent.apply";
        if (auto error = fields(params, applying ? QStringList{"runId", "id", "baseRevision", "digest"} : QStringList{"runId", "proposalId"})) { reply(request.id, *error); return; }
        auto parsedRun = json::revision(params.value("runId"));
        if (auto error = std::get_if<WorkspaceError>(&parsedRun)) { reply(request.id, *error); return; }
        if (!run_.id() || std::get<Revision>(parsedRun) != run_.id() || !agentReviewRequest_.isEmpty()) { fail("runId does not identify an available review"); return; }
        if (!run_.canApply()) { fail("No current revision-bound agent review is available"); return; }
        if (applying) {
            auto approvalFields = params; approvalFields.remove("runId");
            auto parsed = json::approval(approvalFields);
            if (auto error = std::get_if<WorkspaceError>(&parsed)) { reply(request.id, *error); return; }
            const auto approval = std::get<Approval>(parsed);
            const auto &review = *run_.review();
            if (approval.id != review.id || approval.baseRevision != review.baseRevision || approval.digest != review.digest) {
                reply(request.id, WorkspaceError{ErrorCode::StaleApproval, "Approval must match the exact displayed proposal ID, base revision, and digest"}); return;
            }
            agentReviewRequest_ = request.id;
            if (!run_.applyReview(approval) && !agentReviewRequest_.isEmpty()) {
                agentReviewRequest_.clear(); fail("Agent review could not be applied");
            }
        } else {
            auto parsed = proposalId(params.value("proposalId"));
            if (auto error = std::get_if<WorkspaceError>(&parsed)) { reply(request.id, *error); return; }
            const auto id = std::get<ProposalId>(parsed);
            if (id != run_.review()->id) { fail("proposalId does not match the displayed agent review"); return; }
            agentReviewRequest_ = request.id;
            if (!run_.rejectReview(id) && !agentReviewRequest_.isEmpty()) {
                agentReviewRequest_.clear(); fail("Agent review could not be rejected");
            }
        }
    } else fail("Unknown session method: " + request.method);
}
void CliSession::agentEvent(const agent::RunEvent &event) {
    QJsonObject object{{"type", "agentEvent"}, {"runId", QString::number(event.runId)},
        {"sequence", QString::number(event.sequence)}, {"status", agent::statusName(event.status)},
        {"activity", event.activity}, {"transactionId", event.transactionId}};
    if (event.assistantMessage) object.insert("assistantMessage", QJsonObject{{"text", event.assistantMessage->text}});
    if (event.proposal) object.insert("proposal", json::proposal(*event.proposal));
    if (event.error) object.insert("error", errorObject(*event.error));
    writeJson(output_, object);
    if (terminal(event.status) && !agentReviewRequest_.isEmpty()) {
        const auto request = std::exchange(agentReviewRequest_, {});
        if (event.status == agent::RunStatus::Failed && event.error) agentError(request, *event.error);
        else reply(request, QJsonObject{{"runId", QString::number(event.runId)}, {"status", agent::statusName(event.status)}, {"transactionId", event.transactionId}});
    }
    QTimer::singleShot(0, this, &CliSession::drain);
}
void CliSession::finishIfReady() {
    if (!eof_ || finishing_ || pending_ || !requests_.isEmpty()) return;
    if (run_.status() == agent::RunStatus::Applying) return;
    run_.cancel(); // EOF never approves or saves; approved commits were awaited above.
    if (!agentReviewRequest_.isEmpty()) return;
    finishing_ = true;
    emit finished(exitCode_);
}
} // namespace rose::cli
