#pragma once
#include "AgentTypes.h"
namespace rose::agent {
class AgentHistory final {
public:
    static std::optional<AgentError> record(const ProviderConfig &, RunId, RunStatus, const QString &proposalId = {}, const QString &transactionId = {});
};
} // namespace rose::agent
