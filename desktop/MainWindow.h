#pragma once
#include "WorkspaceController.h"
#include <QMainWindow>
#include <QHash>
#include <QPointer>
#include <functional>
class QTreeView;
class QTableView;
class QStandardItemModel;
class QTabWidget;
class QPlainTextEdit;
class QAction;
class QMenu;
namespace rose::agent { class AgentDock; }
namespace rose::desktop {
class DiagramView;
class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    void openPath(const QString &);
protected:
    void closeEvent(QCloseEvent *) override;
private:
    using Callback = std::function<void(const WorkspaceReply &)>;
    void track(quint64, Callback);
    void receive(quint64, const WorkspaceReply &);
    void refresh();
    void populate(const Projection &);
    void refreshDiagram(const DiagramId &);
    void openDiagram(const DiagramId &);
    void inspectObject(const ObjectId &);
    void submit(QVector<Command>);
    bool review(const Proposal &);
    void diagnostic(const Diagnostic &);
    void failure(const WorkspaceError &);
    void afterDirty(std::function<void()>);
    void save(bool saveAs = false, std::function<void()> after = {});
    void newModel();
    void chooseOpen();
    void configureAccess();
    bool choosePolicy(const QString &, AccessPolicy &, bool creating, RoseProfile *profile = nullptr);
    void buildActions();
    void createElement();
    void renameElement();
    void reparentElement(bool copy);
    void deleteElement();
    void createDiagram();
    void addPresentation();
    void removePresentation();
    void createRelation();
    void reconnectRelation();
    void editGeometry();
    void editRoute();
    void align(bool horizontal);
    void editProperty(const QModelIndex &);
    void updateActions();
    void updateRecent(const QString &);
    DiagramView *activeView() const;
    ElementId selectedElement() const;
    std::optional<ObjectId> treeObject(const QModelIndex &) const;
    WorkspaceController *controller_;
    agent::AgentDock *agentDock_;
    Projection tree_;
    AccessPolicy access_;
    QTreeView *browser_;
    QTableView *inspector_;
    QStandardItemModel *treeModel_, *propertyModel_;
    QTabWidget *tabs_;
    QPlainTextEdit *documentation_, *diagnostics_;
    QHash<quint64, Callback> pending_;
    QHash<DiagramId, QPointer<DiagramView>> diagrams_;
    std::optional<ObjectId> inspected_;
    bool dirty_ = false, closing_ = false, fileBusy_ = false, propertyLoading_ = false;
    QAction *saveAction_, *saveAsAction_, *undoAction_, *redoAction_;
    QVector<QAction *> modelActions_;
    QMenu *recentMenu_;
};
}
