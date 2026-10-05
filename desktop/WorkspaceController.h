#pragma once

#include "core/Workspace.h"
#include <QObject>
#include <QThread>
#include <QHash>
#include <functional>

namespace rose::desktop {

class WorkspaceWorker;
// All methods execute on the GUI thread. The worker exclusively owns Workspace.
class WorkspaceController final : public QObject {
    Q_OBJECT
public:
    explicit WorkspaceController(QObject *parent = nullptr);
    ~WorkspaceController() override;
    quint64 open(const QString &, const AccessPolicy &);
    quint64 create(const QString &, RoseProfile, const AccessPolicy &);
    quint64 inspect(const Query &);
    quint64 propose(Revision, const QVector<Command> &);
    quint64 apply(const Approval &);
    quint64 reject(const ProposalId &);
    quint64 undo();
    quint64 redo();
    quint64 save(const SaveMode & = {});
    quint64 close();
    quint64 grantDirectory(const QString &);
    Revision revision() const noexcept { return revision_; }
    bool hasWorkspace() const noexcept { return hasWorkspace_; }
    quint64 sessionGeneration() const noexcept { return sessionGeneration_; }
signals:
    void completed(quint64 requestId, rose::WorkspaceReply reply);
    void workspaceChanged(quint64 generation);
private:
    quint64 enqueue(std::function<WorkspaceReply(std::unique_ptr<Workspace> &)>);
    QThread thread_;
    WorkspaceWorker *worker_ = nullptr; // QObject-owned; destroyed on worker thread.
    quint64 sequence_ = 0;
    quint64 sessionGeneration_ = 0;
    QHash<quint64, quint64> requestGenerations_;
    Revision revision_ = 0;
    bool hasWorkspace_ = false;
};

} // namespace rose::desktop
