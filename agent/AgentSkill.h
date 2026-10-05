#pragma once
#include "AgentTypes.h"

namespace rose::agent {
// Only the compiled resource is trusted; there is no user-path/settings override.
std::variant<QString, AgentError> softwareEngineeringSkill();
} // namespace rose::agent
