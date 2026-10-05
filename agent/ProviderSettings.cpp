#include "ProviderSettings.h"
#include <QSettings>
#include <limits>

namespace rose::agent {
namespace {
QString key(const char *name) { return QStringLiteral("agent/provider/") + QLatin1String(name); }
AgentError invalid() { return {AgentErrorCode::Configuration, QStringLiteral("Provider settings are missing or invalid. Configure the endpoint, model and OS credential reference.")}; }
bool referenceValid(const QString &reference) {
    if (reference.isEmpty() || reference.size() > 200) return false;
    for (const auto c : reference) if (!c.isLetterOrNumber() && c != '-' && c != '_' && c != '.') return false;
    return true;
}
std::optional<AgentError> validate(const ProviderConfig &config) {
    if (auto error = validateConfig(config)) return error;
    if ((!config.credentialReference.isEmpty() && !referenceValid(config.credentialReference))
        || (config.kind == ProviderKind::Hosted && config.credentialReference.isEmpty())) return invalid();
    return {};
}
}
std::variant<ProviderConfig, AgentError> ProviderSettings::load(const QSettings &settings) {
    ProviderConfig config;
    const auto kind = settings.value(key("kind")).toString();
    if (kind == "hosted") config.kind = ProviderKind::Hosted;
    else if (kind == "local") config.kind = ProviderKind::Local;
    else return invalid();
    config.endpoint = QUrl(settings.value(key("endpoint")).toString(), QUrl::StrictMode);
    config.model = settings.value(key("model")).toString();
    config.credentialReference = settings.value(key("credentialReference")).toString();
    const std::pair<const char *, int *> budgets[] = {
        {"timeoutMs", &config.timeoutMs}, {"maxRounds", &config.maxRounds},
        {"maxToolCalls", &config.maxToolCalls}, {"maxContextBytes", &config.maxContextBytes},
        {"maxResponseBytes", &config.maxResponseBytes}};
    for (const auto &[name, target] : budgets) {
        bool ok = false;
        const auto value = settings.value(key(name)).toString().toLongLong(&ok);
        if (!ok || value < 0 || value > std::numeric_limits<int>::max()) return invalid();
        *target = int(value);
    }
    if (settings.contains(key("contextWindowTokens"))) {
        bool ok = false;
        config.contextWindowTokens = settings.value(key("contextWindowTokens")).toString().toLongLong(&ok);
        if (!ok || config.contextWindowTokens < 0) return invalid();
    }
    if (auto error = validate(config)) return *error;
    return config;
}
std::optional<AgentError> ProviderSettings::store(QSettings &settings, const ProviderConfig &config) {
    if (auto error = validate(config)) return error;
    settings.setValue(key("kind"), config.kind == ProviderKind::Hosted ? "hosted" : "local");
    settings.setValue(key("endpoint"), config.endpoint.toString(QUrl::FullyEncoded));
    settings.setValue(key("model"), config.model);
    settings.setValue(key("credentialReference"), config.credentialReference);
    settings.setValue(key("timeoutMs"), config.timeoutMs);
    settings.setValue(key("maxRounds"), config.maxRounds);
    settings.setValue(key("maxToolCalls"), config.maxToolCalls);
    settings.setValue(key("maxContextBytes"), config.maxContextBytes);
    settings.setValue(key("maxResponseBytes"), config.maxResponseBytes);
    settings.setValue(key("contextWindowTokens"), config.contextWindowTokens);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return AgentError{AgentErrorCode::Configuration, QStringLiteral("Provider settings could not be saved. No key is stored in these settings.")};
    return {};
}
} // namespace rose::agent
