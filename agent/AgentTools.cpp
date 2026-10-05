#include "AgentTools.h"
#include "core/Json.h"
#include <QJsonDocument>
#include <QJsonParseError>
namespace rose::agent {
ToolAction AgentTools::parse(const ToolCall &call, Revision observedRevision) {
    if (call.name != "inspect" && call.name != "submit_proposal")
        return AgentError{AgentErrorCode::PermissionDenied, "Tool is not permitted. Agent capabilities are inspect and submit_proposal only."};
    QJsonParseError syntax;
    const auto document = QJsonDocument::fromJson(call.arguments, &syntax);
    if (syntax.error != QJsonParseError::NoError || !document.isObject())
        return AgentError{AgentErrorCode::InvalidArguments, "Tool arguments must be a valid JSON object."};
    const auto object = document.object();
    if (call.name == "inspect") {
        const auto result = json::query(object);
        if (auto error = std::get_if<WorkspaceError>(&result)) return AgentError{AgentErrorCode::InvalidArguments, error->message};
        return std::get<Query>(result);
    }
    if (object.size() != 2 || !object.contains("baseRevision") || !object.value("commands").isArray())
        return AgentError{AgentErrorCode::InvalidArguments, "submit_proposal accepts exactly baseRevision and commands."};
    const auto revision = json::revision(object.value("baseRevision"));
    if (auto error = std::get_if<WorkspaceError>(&revision)) return AgentError{AgentErrorCode::InvalidArguments, error->message};
    const auto base = std::get<Revision>(revision);
    if (base != observedRevision) return AgentError{AgentErrorCode::StaleRevision, "Proposal base revision differs from the run's observed revision. Start a new run."};
    const auto commands = json::commands(object.value("commands").toArray());
    if (auto error = std::get_if<WorkspaceError>(&commands)) return AgentError{AgentErrorCode::InvalidArguments, error->message};
    return PreparedProposal{base, std::get<QVector<Command>>(commands)};
}
std::variant<quint64, AgentError> AgentTools::dispatch(const ToolAction &action) const {
    if (auto error = std::get_if<AgentError>(&action)) return *error;
    if (!controller_ || !controller_->hasWorkspace())
        return AgentError{AgentErrorCode::WorkspaceUnavailable, "Open a workspace before using agent tools."};
    if (auto query = std::get_if<Query>(&action)) return controller_->inspect(*query);
    const auto &proposal = std::get<PreparedProposal>(action);
    return controller_->propose(proposal.base, proposal.commands);
}
} // namespace rose::agent
