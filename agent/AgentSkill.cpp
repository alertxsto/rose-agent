#include "AgentSkill.h"
#include <QFile>
#include <QResource>

static void initializeAgentSkills() { Q_INIT_RESOURCE(skills); }

namespace rose::agent {
std::variant<QString, AgentError> softwareEngineeringSkill() {
    static const bool initialized = [] { initializeAgentSkills(); return true; }();
    Q_UNUSED(initialized);
    QFile file(":/rose-agent/skills/software-engineering.md");
    if (!file.open(QIODevice::ReadOnly))
        return AgentError{AgentErrorCode::Configuration, "The bundled software-engineering skill is unavailable."};
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError || bytes.isEmpty())
        return AgentError{AgentErrorCode::Configuration, "The bundled software-engineering skill could not be read."};
    return QString::fromUtf8(bytes);
}
} // namespace rose::agent
