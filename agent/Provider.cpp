#include "Provider.h"
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QSet>
#include <QThread>
#include <QTimer>

namespace rose::agent {
struct Provider::Pending {
    QPointer<QNetworkReply> reply;
    QTimer *timer = nullptr;
    QByteArray buffer;
    QByteArray eventData;
    QMap<int, ToolCall> calls;
    ProviderResponse response;
    qsizetype received = 0;
    qsizetype maximum = 0;
    bool streaming = true;
    bool done = false;
    bool finishedChoice = false;
    QString finishReason;
    std::optional<AgentError> error;
};
Provider::Provider(QObject *parent) : QObject(parent) { qRegisterMetaType<ProviderResult>(); }
Provider::~Provider() { cancelAll(); }
RequestId Provider::complete(const ProviderConfig &config, const QJsonArray &messages, const QJsonArray &tools, const QByteArray &key) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto id = ++sequence_;
    auto deferredError = [this, id](AgentError error) {
        auto pending = std::make_unique<Pending>();
        pending_[id] = std::move(pending);
        QTimer::singleShot(0, this, [this, id, error] { fail(id, error); });
    };
    if (auto error = validateConfig(config)) { deferredError(*error); return id; }
    if (config.kind == ProviderKind::Hosted && key.isEmpty()) {
        deferredError({AgentErrorCode::Credentials, "Hosted provider credentials are missing; configure the OS credential store."}); return id;
    }
    const QJsonObject payload{{"model", config.model}, {"messages", messages}, {"tools", tools}, {"stream", true}, {"tool_choice", "auto"}};
    const auto bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    if (bytes.size() > config.maxContextBytes) {
        deferredError({AgentErrorCode::ContextBudgetExceeded, "Request context exceeds the configured byte budget; narrow the request."}); return id;
    }
    auto endpoint = config.endpoint;
    // Avoid a DNS/proxy escape for the special loopback host.
    if (config.kind == ProviderKind::Local && endpoint.host() == "localhost") endpoint.setHost("127.0.0.1");
    QNetworkRequest request(endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "text/event-stream, application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (!key.isEmpty()) request.setRawHeader("Authorization", "Bearer " + key);
    if (config.kind == ProviderKind::Local) network_.setProxy(QNetworkProxy::NoProxy);
    else network_.setProxy(QNetworkProxy::DefaultProxy);
    auto state = std::make_unique<Pending>();
    state->maximum = config.maxResponseBytes;
    state->reply = network_.post(request, bytes);
    state->timer = new QTimer(this);
    state->timer->setSingleShot(true);
    auto *reply = state->reply.data();
    auto *timer = state->timer;
    pending_[id] = std::move(state);
    connect(timer, &QTimer::timeout, this, [this, id] { fail(id, {AgentErrorCode::Timeout, "Provider request timed out. No model changes were applied."}); });
    connect(reply, &QNetworkReply::readyRead, this, [this, id] { consume(id); });
    connect(reply, &QNetworkReply::finished, this, [this, id] { finish(id); });
    timer->start(config.timeoutMs);
    return id;
}
void Provider::cancel(RequestId id) {
    Q_ASSERT(QThread::currentThread() == thread());
    auto found = pending_.find(id);
    if (found == pending_.end()) return;
    auto state = std::move(found->second);
    pending_.erase(found); // Remove identity before abort emits synchronous signals.
    if (state->timer) { state->timer->stop(); state->timer->deleteLater(); }
    if (state->reply) { state->reply->disconnect(this); state->reply->abort(); state->reply->deleteLater(); }
}
void Provider::cancelAll() { while (!pending_.empty()) cancel(pending_.begin()->first); }
void Provider::fail(RequestId id, AgentError error) {
    if (!pending_.contains(id)) return;
    cancel(id);
    emit completed(id, std::move(error));
}
bool Provider::parseJson(Pending &state, const QJsonObject &object, bool streaming) {
    if (object.contains("error")) return false;
    if (object.value("id").isString()) state.response.providerId = object.value("id").toString();
    if (!object.value("choices").isArray()) return false;
    const auto choices = object.value("choices").toArray();
    // Usage-only trailing events are legal; only one assistant choice is requested.
    if (choices.isEmpty()) return streaming;
    if (choices.size() != 1 || !choices.first().isObject()) return false;
    const auto choice = choices.first().toObject();
    if (choice.contains("index") && choice.value("index").toInt(-1) != 0) return false;
    const auto value = choice.value(streaming ? "delta" : "message");
    if (!value.isObject()) return false;
    const auto delta = value.toObject();
    if (delta.contains("role") && delta.value("role").toString() != "assistant") return false;
    if (delta.contains("content") && !delta.value("content").isNull()) {
        if (!delta.value("content").isString()) return false;
        state.response.text += delta.value("content").toString();
    }
    if (delta.contains("function_call")) return false; // Legacy unidentifiable tool calls are unsafe.
    if (delta.contains("tool_calls")) {
        if (!delta.value("tool_calls").isArray()) return false;
        const auto calls = delta.value("tool_calls").toArray();
        for (qsizetype position = 0; position < calls.size(); ++position) {
            if (!calls[position].isObject()) return false;
            const auto fragment = calls[position].toObject();
            const auto indexValue = fragment.value("index");
            const auto index = streaming ? indexValue.toInt(-1) : int(position);
            if (index < 0 || index > 1023 || (streaming && indexValue.toDouble(-1) != index)) return false;
            auto &call = state.calls[index];
            if (fragment.contains("id")) {
                if (!fragment.value("id").isString()) return false;
                call.id += fragment.value("id").toString();
            }
            if (fragment.contains("type") && fragment.value("type").toString() != "function") return false;
            if (fragment.contains("function")) {
                if (!fragment.value("function").isObject()) return false;
                const auto function = fragment.value("function").toObject();
                if (function.contains("name")) {
                    if (!function.value("name").isString()) return false;
                    call.name += function.value("name").toString();
                }
                if (function.contains("arguments")) {
                    if (!function.value("arguments").isString()) return false;
                    call.arguments += function.value("arguments").toString().toUtf8();
                }
            }
        }
    }
    const auto reason = choice.value("finish_reason");
    if (!reason.isUndefined() && !reason.isNull()) {
        if (!reason.isString() || state.finishedChoice) return false;
        state.finishReason = reason.toString();
        state.finishedChoice = true;
    }
    return true;
}
bool Provider::parseEvent(Pending &state, const QByteArray &event) {
    if (event.trimmed() == "[DONE]") { state.done = true; return true; }
    if (state.done) return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(event, &error);
    return error.error == QJsonParseError::NoError && document.isObject() && parseJson(state, document.object(), true);
}
void Provider::consume(RequestId id) {
    auto found = pending_.find(id);
    if (found == pending_.end() || !found->second->reply) return;
    auto &state = *found->second;
    const auto chunk = state.reply->readAll();
    state.received += chunk.size();
    if (state.received > state.maximum) { fail(id, {AgentErrorCode::ContextBudgetExceeded, "Provider response exceeds the configured byte budget."}); return; }
    state.buffer += chunk;
    const auto contentType = state.reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
    state.streaming = contentType.startsWith("text/event-stream");
    if (!state.streaming) return;
    while (true) {
        auto newline = state.buffer.indexOf('\n');
        if (newline < 0) break;
        auto line = state.buffer.left(newline);
        state.buffer.remove(0, newline + 1);
        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty()) {
            if (!state.eventData.isEmpty()) {
                const auto valid = parseEvent(state, state.eventData);
                state.eventData.clear();
                if (!valid) { fail(id, {AgentErrorCode::Protocol, "Malformed streaming provider response or unsupported tool-call protocol."}); return; }
            }
        } else if (line.startsWith("data:")) {
            auto data = line.mid(5); if (data.startsWith(' ')) data.remove(0, 1);
            if (!state.eventData.isEmpty()) state.eventData += '\n';
            state.eventData += data;
        }
    }
}
void Provider::finish(RequestId id) {
    auto found = pending_.find(id);
    if (found == pending_.end() || !found->second->reply) return;
    const auto status = found->second->reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 300 && status < 400) { fail(id, {AgentErrorCode::Network, "Provider redirect refused. Configure the final endpoint explicitly."}); return; }
    if (status == 401 || status == 403) { fail(id, {AgentErrorCode::Authentication, "Provider rejected authentication. Check the credential and endpoint."}); return; }
    if (status == 429) { fail(id, {AgentErrorCode::Network, "Provider rate limit reached. No automatic retry was made."}); return; }
    if (status < 200 || status >= 300 || found->second->reply->error() != QNetworkReply::NoError) {
        fail(id, {AgentErrorCode::Network, QString("Provider request failed (HTTP %1). Check network, TLS, endpoint, and service availability.").arg(status)}); return;
    }
    consume(id);
    found = pending_.find(id); if (found == pending_.end()) return;
    auto &state = *found->second;
    if (!state.streaming) {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(state.buffer, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject() || !parseJson(state, document.object(), false)) {
            fail(id, {AgentErrorCode::Protocol, "Malformed provider JSON or unsupported tool-call protocol."}); return;
        }
        state.done = true;
    }
    if (!state.done || !state.finishedChoice || !state.buffer.trimmed().isEmpty() && state.streaming
        || (state.finishReason != "stop" && state.finishReason != "tool_calls")) {
        fail(id, {AgentErrorCode::Protocol, "Provider response was incomplete, filtered, or exceeded its generation limit."}); return;
    }
    QSet<QString> ids;
    for (auto it = state.calls.cbegin(); it != state.calls.cend(); ++it) {
        const auto &call = it.value();
        if (call.id.isEmpty() || call.name.isEmpty() || call.arguments.isEmpty() || ids.contains(call.id)) {
            fail(id, {AgentErrorCode::Protocol, "Provider tool call has missing or duplicate identity/arguments."}); return;
        }
        ids.insert(call.id); state.response.calls.append(call);
    }
    if ((state.finishReason == "tool_calls") != !state.response.calls.isEmpty()) {
        fail(id, {AgentErrorCode::Protocol, "Provider tool-call finish reason does not match its calls."}); return;
    }
    auto response = std::move(state.response);
    cancel(id);
    emit completed(id, std::move(response));
}
} // namespace rose::agent
