#pragma once

#include "core/Workspace.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

namespace rose::agent {
using RequestId = quint64;
using RunId = quint64;
enum class ProviderKind { Local, Hosted };
enum class AgentErrorCode {
    Configuration, Credentials, Authentication, Network, Timeout, Protocol,
    PermissionDenied, InvalidArguments, ContextBudgetExceeded, CallBudgetExceeded,
    StaleRevision, WorkspaceUnavailable
};
struct AgentError { AgentErrorCode code; QString message; };
struct AgentImage { QString mimeType; QByteArray bytes; };
struct AgentInput { QString prompt; QVector<AgentImage> images; };
std::variant<AgentImage, AgentError> loadImage(const QString &path);
std::optional<AgentError> validateInput(const AgentInput &, int maxContextBytes);
struct ProviderConfig {
    ProviderKind kind = ProviderKind::Local;
    QUrl endpoint;
    QString model;
    QString credentialReference;
    int timeoutMs = 120000;
    int maxRounds = 16;
    int maxToolCalls = 64;
    int maxContextBytes = 1024 * 1024;
    int maxResponseBytes = 8 * 1024 * 1024;
    // Advertised by the provider's model catalog; 0 means not advertised.
    // This is a token count, not the independent transport-memory byte bound.
    qint64 contextWindowTokens = 0;
};
struct ToolCall { QString id, name; QByteArray arguments; };
struct ProviderResponse {
    QString text;
    QVector<ToolCall> calls;
    QString providerId;
    QJsonObject assistantMessage() const;
};
using ProviderResult = std::variant<ProviderResponse, AgentError>;
enum class RunStatus { Planning, Reading, Proposing, AwaitingReview, Applying, Completed, Cancelled, Failed };
struct AssistantMessage { QString text; };
struct RunEvent {
    RunId runId = 0;
    quint64 sequence = 0;
    RunStatus status = RunStatus::Planning;
    QString activity;
    std::optional<Proposal> proposal;
    std::optional<AgentError> error;
    QString transactionId;
    std::optional<AssistantMessage> assistantMessage;
};
std::optional<AgentError> validateConfig(const ProviderConfig &);
QString errorName(AgentErrorCode);
QString statusName(RunStatus);
} // namespace rose::agent
Q_DECLARE_METATYPE(rose::agent::ProviderResult)
Q_DECLARE_METATYPE(rose::agent::RunEvent)
