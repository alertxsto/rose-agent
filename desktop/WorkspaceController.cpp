#include "WorkspaceController.h"
#include "WorkspaceWorker.h"
#include <QMetaObject>
#include <limits>
namespace rose::desktop {
namespace {
WorkspaceError missing() { return {ErrorCode::InvalidCommand, QStringLiteral("No workspace is open")}; }
template<class T> WorkspaceReply reply(Outcome<T> result) {
    if (auto *error = std::get_if<WorkspaceError>(&result)) return *error;
    return std::move(std::get<T>(result));
}
WorkspaceReply opened(Outcome<std::unique_ptr<Workspace>> result, std::unique_ptr<Workspace> &workspace,
                      WorkspaceWorker *worker, quint64 generation) {
    if (auto *error = std::get_if<WorkspaceError>(&result)) return *error;
    auto next = std::move(std::get<std::unique_ptr<Workspace>>(result));
    Query query; query.limit = std::numeric_limits<qsizetype>::max();
    auto projection = next->inspect(query);
    if (auto *error = std::get_if<WorkspaceError>(&projection)) return *error;
    if (worker->requestedGeneration() != generation)
        return WorkspaceError{ErrorCode::StaleRevision, QStringLiteral("Workspace request was superseded")};
    workspace = std::move(next);
    return std::move(std::get<Projection>(projection));
}
}
WorkspaceController::WorkspaceController(QObject *parent) : QObject(parent), worker_(new WorkspaceWorker) {
    qRegisterMetaType<rose::WorkspaceReply>();
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &WorkspaceWorker::finished, this, [this](quint64 request, WorkspaceReply value) {
        const auto generation = requestGenerations_.take(request);
        if (generation != sessionGeneration_) {
            emit completed(request, WorkspaceError{ErrorCode::StaleRevision,
                QStringLiteral("Result belongs to a superseded workspace session")});
            return;
        }
        if (auto *projection = std::get_if<Projection>(&value)) { revision_ = projection->revision; hasWorkspace_ = true; }
        else if (auto *applied = std::get_if<Applied>(&value)) revision_ = applied->revision;
        else if (auto *saved = std::get_if<SaveReceipt>(&value)) revision_ = saved->revision;
        else if (std::holds_alternative<Closed>(value)) { revision_ = 0; hasWorkspace_ = false; }
        else if (std::holds_alternative<AccessGranted>(value)) {
            ++sessionGeneration_;
            worker_->setRequestedGeneration(sessionGeneration_);
            emit workspaceChanged(sessionGeneration_);
        }
        emit completed(request, std::move(value));
    }, Qt::QueuedConnection);
    thread_.setObjectName(QStringLiteral("Workspace"));
    thread_.start();
}
WorkspaceController::~WorkspaceController() {
    disconnect(worker_, nullptr, this, nullptr);
    thread_.quit();
    thread_.wait();
}
quint64 WorkspaceController::enqueue(std::function<WorkspaceReply(std::unique_ptr<Workspace> &)> operation) {
    Q_ASSERT(QThread::currentThread() == thread());
    const quint64 request = ++sequence_;
    const auto generation = sessionGeneration_;
    requestGenerations_.insert(request, generation);
    QMetaObject::invokeMethod(worker_, [worker = worker_, request, generation, operation = std::move(operation)]() mutable {
        if (worker->requestedGeneration() != generation) {
            emit worker->finished(request, WorkspaceError{ErrorCode::StaleRevision, QStringLiteral("Workspace request was superseded")});
            return;
        }
        worker->execute(request, std::move(operation));
    }, Qt::QueuedConnection);
    return request;
}
quint64 WorkspaceController::open(const QString &path, const AccessPolicy &access) {
    const auto generation = ++sessionGeneration_;
    worker_->setRequestedGeneration(generation);
    emit workspaceChanged(generation);
    return enqueue([path, access, worker = worker_, generation](auto &workspace) {
        return opened(Workspace::open(path, access), workspace, worker, generation);
    });
}
quint64 WorkspaceController::create(const QString &path, RoseProfile profile, const AccessPolicy &access) {
    const auto generation = ++sessionGeneration_;
    worker_->setRequestedGeneration(generation);
    emit workspaceChanged(generation);
    return enqueue([path, profile, access, worker = worker_, generation](auto &workspace) {
        return opened(Workspace::create(path, profile, access), workspace, worker, generation);
    });
}
quint64 WorkspaceController::inspect(const Query &query) {
    return enqueue([query](auto &workspace) -> WorkspaceReply { return workspace ? reply(workspace->inspect(query)) : WorkspaceReply{missing()}; });
}
quint64 WorkspaceController::propose(Revision base, const QVector<Command> &commands) {
    return enqueue([base, commands](auto &workspace) -> WorkspaceReply { return workspace ? reply(workspace->propose(base, commands)) : WorkspaceReply{missing()}; });
}
quint64 WorkspaceController::apply(const Approval &approval) {
    return enqueue([approval](auto &workspace) -> WorkspaceReply { return workspace ? reply(workspace->apply(approval)) : WorkspaceReply{missing()}; });
}
quint64 WorkspaceController::reject(const ProposalId &id) {
    return enqueue([id](auto &workspace) -> WorkspaceReply {
        if (!workspace) return missing();
        auto result = workspace->reject(id);
        if (auto *error = std::get_if<WorkspaceError>(&result)) return *error;
        return Rejected{id};
    });
}
quint64 WorkspaceController::undo() { return enqueue([](auto &w) -> WorkspaceReply { return w ? reply(w->undo()) : WorkspaceReply{missing()}; }); }
quint64 WorkspaceController::redo() { return enqueue([](auto &w) -> WorkspaceReply { return w ? reply(w->redo()) : WorkspaceReply{missing()}; }); }
quint64 WorkspaceController::save(const SaveMode &mode) { return enqueue([mode](auto &w) -> WorkspaceReply { return w ? reply(w->save(mode)) : WorkspaceReply{missing()}; }); }
quint64 WorkspaceController::close() {
    ++sessionGeneration_;
    worker_->setRequestedGeneration(sessionGeneration_);
    emit workspaceChanged(sessionGeneration_);
    return enqueue([](auto &workspace) -> WorkspaceReply { workspace.reset(); return Closed{}; });
}
quint64 WorkspaceController::grantDirectory(const QString &directory) {
    return enqueue([directory](auto &workspace) -> WorkspaceReply {
        if (!workspace) return missing();
        auto result = workspace->grantDirectory(directory);
        if (auto *error = std::get_if<WorkspaceError>(&result)) return *error;
        return AccessGranted{directory};
    });
}
}
