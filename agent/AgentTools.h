#pragma once
#include "AgentTypes.h"
#include "desktop/WorkspaceController.h"
#include <QPointer>
namespace rose::agent {
struct PreparedProposal { Revision base; QVector<Command> commands; };
using ToolAction = std::variant<Query, PreparedProposal, AgentError>;
class AgentTools final {
public:
    explicit AgentTools(desktop::WorkspaceController *controller) : controller_(controller) {}
    static ToolAction parse(const ToolCall &, Revision observedRevision);
    std::variant<quint64, AgentError> dispatch(const ToolAction &) const;
private:
    QPointer<desktop::WorkspaceController> controller_;
};
} // namespace rose::agent
