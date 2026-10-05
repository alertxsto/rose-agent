#include "AgentHistory.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
namespace rose::agent {
std::optional<AgentError> AgentHistory::record(const ProviderConfig &config, RunId run, RunStatus status, const QString &proposal, const QString &transaction) {
    const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/agent-history";
    if (!QDir().mkpath(directory) || !QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
        return AgentError{AgentErrorCode::Configuration, "Could not secure local agent history directory."};
    QJsonArray entries;
    QFile previous(directory + "/history.json");
    if (previous.exists()) {
        if (!previous.open(QIODevice::ReadOnly) || previous.size() > 1024 * 1024)
            return AgentError{AgentErrorCode::Configuration, "Could not read bounded local agent history."};
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(previous.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isArray())
            return AgentError{AgentErrorCode::Configuration, "Local agent history is invalid; model data is unaffected."};
        entries = document.array();
    }
    while (entries.size() >= 500) entries.removeFirst();
    // No prompts, model content, keys, endpoints with credentials, or hidden reasoning.
    entries.append(QJsonObject{{"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}, {"runId", QString::number(run)},
        {"status", statusName(status)}, {"provider", config.kind == ProviderKind::Local ? "local" : "hosted"},
        {"model", config.model}, {"proposalId", proposal}, {"transactionId", transaction}});
    QSaveFile file(directory + "/history.json");
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return AgentError{AgentErrorCode::Configuration, "Could not open restrictive local agent history."};
    const auto bytes = QJsonDocument(entries).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit())
        return AgentError{AgentErrorCode::Configuration, "Could not persist local agent history; model data is unaffected."};
    return std::nullopt;
}
} // namespace rose::agent
