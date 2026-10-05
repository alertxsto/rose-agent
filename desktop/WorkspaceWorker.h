#pragma once
#include "core/Workspace.h"
#include <QObject>
#include <functional>
#include <atomic>
namespace rose::desktop {
class WorkspaceWorker final : public QObject {
    Q_OBJECT
public:
    using Operation = std::function<WorkspaceReply(std::unique_ptr<Workspace> &)>;
    void execute(quint64 request, Operation operation);
    void setRequestedGeneration(quint64 generation) { generation_.store(generation); }
    quint64 requestedGeneration() const { return generation_.load(); }
signals:
    void finished(quint64 request, rose::WorkspaceReply reply);
private:
    std::unique_ptr<Workspace> workspace_;
    std::atomic<quint64> generation_{0};
};
}
