#pragma once
#include "AgentTools.h"
#include "Provider.h"
#include "Credentials.h"
#include <QPointer>
#include <QFutureWatcher>

namespace rose::agent {
// GUI-thread coordinator; Workspace remains exclusively inside its controller worker.
class AgentRun final : public QObject {
    Q_OBJECT
public:
    explicit AgentRun(desktop::WorkspaceController *, QObject *parent = nullptr);
    ~AgentRun() override;
    RunId start(const ProviderConfig &, const AgentInput &, const QVector<ElementId> &selectedIds = {});
    void cancel();
    void resetConversation(); // In-memory design context only; never persisted with credentials or tool data.
    bool applyReview(const Approval &displayed); // Explicit exact-tuple UI/CLI approval, never a provider capability.
    bool rejectReview(const ProposalId &displayed);
    bool active() const noexcept;
    RunStatus status() const noexcept { return status_; }
    RunId id() const noexcept { return runId_; }
    const std::optional<Proposal> &review() const noexcept { return review_; }
    bool canApply() const noexcept;
signals:
    void event(rose::agent::RunEvent);
private:
    enum class WorkspaceTask { None, InitialContext, Inspect, Propose, Apply, Reject };
    void providerCompleted(RequestId, ProviderResult);
    void workspaceCompleted(quint64, const WorkspaceReply &);
    bool fresh();
    void beginContext(const QVector<ElementId> &selectedIds);
    void cancelCredentials();
    void requestProvider();
    void nextTool();
    void appendTool(const QJsonObject &);
    void report(RunStatus, const QString & = {}, std::optional<AgentError> = {}, const QString &transaction = {});
    void fail(AgentError);
    void discardPendingProposal();
    void finishHistory(const QString &transaction = {});
    void remember(const QJsonObject &);
    QPointer<desktop::WorkspaceController> controller_;
    Provider provider_;
    AgentTools tools_;
    ProviderConfig config_;
    QByteArray key_;
    QPointer<QFutureWatcher<std::shared_ptr<CredentialLookup>>> credentialWatcher_;
    QJsonArray messages_;
    QJsonArray conversation_;
    quint64 conversationGeneration_ = 0;
    qsizetype conversationBytes_ = 2;
    bool conversationOverflow_ = false;
    QVector<ToolCall> calls_;
    qsizetype callIndex_ = 0;
    int rounds_ = 0, toolCount_ = 0;
    RunId runId_ = 0;
    quint64 eventSequence_ = 0, generation_ = 0, workspaceRequest_ = 0;
    RequestId providerRequest_ = 0;
    Revision observedRevision_ = 0;
    WorkspaceTask workspaceTask_ = WorkspaceTask::None;
    RunStatus status_ = RunStatus::Completed;
    std::optional<Proposal> review_;
    std::optional<Approval> approval_;
    QString proposalId_;
};
} // namespace rose::agent
