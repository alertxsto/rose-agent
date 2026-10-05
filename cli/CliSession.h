#pragma once
#include "agent/AgentRun.h"
#include "desktop/WorkspaceController.h"
#include <QFile>
#include <QQueue>
#include <QSet>
#include <memory>

namespace rose::cli {
class StdinReader;
class CliSession final : public QObject {
    Q_OBJECT
public:
    CliSession(AccessPolicy, QFile &output, QFile &errors, QObject *parent = nullptr);
    ~CliSession() override;
    void start(const QString &file = {});
signals:
    void finished(int exitCode);
private:
    struct Request { QString id, method; QJsonObject params; };
    void readLine(const QByteArray &);
    void endInput(const QString &);
    void drain();
    void dispatch(const Request &);
    void dispatchAgent(const Request &);
    void workspaceCompleted(quint64, const WorkspaceReply &);
    void agentEvent(const agent::RunEvent &);
    void reply(const QString &, const WorkspaceReply &);
    void reply(const QString &, const QJsonObject &, bool ok = true);
    void agentError(const QString &, const agent::AgentError &);
    bool writeJson(QFile &, const QJsonObject &);
    void finishIfReady();
    void startReader();
    AccessPolicy policy_;
    QFile &output_, &errors_;
    desktop::WorkspaceController controller_;
    agent::AgentRun run_;
    std::unique_ptr<StdinReader> input_;
    QQueue<Request> requests_;
    QSet<QString> inFlight_;
    std::optional<Request> pending_;
    quint64 workspaceRequest_ = 0, startupRequest_ = 0;
    QString agentReviewRequest_;
    bool checkingClose_ = false, eof_ = false, finishing_ = false, outputFailed_ = false;
    int exitCode_ = 0;
};
} // namespace rose::cli
