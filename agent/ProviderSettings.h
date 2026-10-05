#pragma once
#include "AgentTypes.h"
class QSettings;
namespace rose::agent {
class ProviderSettings final {
public:
    static std::variant<ProviderConfig, AgentError> load(const QSettings &);
    static std::optional<AgentError> store(QSettings &, const ProviderConfig &);
};
} // namespace rose::agent
