#pragma once
#include "AgentRun.h"
#include <QDockWidget>
class QDialog;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QListWidget;
class QTableView;
class QStandardItemModel;
class QScrollArea;
class QVBoxLayout;
namespace rose::agent {
class AgentDock final : public QDockWidget {
    Q_OBJECT
public:
    explicit AgentDock(desktop::WorkspaceController *, QWidget *parent = nullptr);
    void setWorkspaceState(bool fileBusy, bool writable);
    void setSelectedElements(QVector<ElementId>);
    bool busy() const;
signals:
    void newModelRequested();
    void openModelRequested();
    void workflowStateChanged();
protected:
    bool eventFilter(QObject *, QEvent *) override;
private:
    void updateControls();
    void configureProvider();
    void attachImage();
    void removeImage();
    void start();
    void receive(const RunEvent &);
    void display(const Proposal &);
    void addMessage(const QString &role, const QString &text, bool user = false);
    void clearConversation();
    void refreshProvider();
    desktop::WorkspaceController *controller_;
    AgentRun *run_;
    QLabel *empty_, *status_, *tuple_, *provider_, *workspace_, *reviewSummary_;
    QPlainTextEdit *prompt_, *activity_, *diagnostics_;
    QPushButton *new_, *open_, *configure_, *clear_, *attach_, *remove_, *start_, *cancel_, *apply_, *reject_;
    QListWidget *attachments_;
    QScrollArea *transcript_;
    QWidget *messages_, *reviewCard_;
    QVBoxLayout *messageLayout_;
    QDialog *details_, *activityDialog_;
    QTableView *diff_;
    QStandardItemModel *diffModel_;
    QVector<AgentImage> images_;
    QVector<ElementId> selected_;
    std::optional<Approval> displayed_;
    quint64 sequence_ = 0, workspaceGeneration_ = 0;
    RunStatus lastStatus_ = RunStatus::Completed;
    bool fileBusy_ = false, writable_ = true, followTail_ = true;
};
}
