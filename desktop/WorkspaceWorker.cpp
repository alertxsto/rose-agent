#include "WorkspaceWorker.h"
#include <QThread>
namespace rose::desktop {
void WorkspaceWorker::execute(quint64 request, Operation operation) {
    Q_ASSERT(QThread::currentThread() == thread());
    emit finished(request, operation(workspace_));
}
}
