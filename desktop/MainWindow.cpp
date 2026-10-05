#include "MainWindow.h"
#include "DiagramScene.h"
#include "Printing.h"
#include "agent/AgentDock.h"
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsItem>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QToolBar>
#include <QTreeView>
#include <QVBoxLayout>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>
namespace rose::desktop {
namespace {
constexpr int IdRole = Qt::UserRole + 1, TypeRole = Qt::UserRole + 2;
QString fresh() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString idText(const ObjectId &id) { return std::visit([](const auto &v) { return v.value; }, id); }
Query allTree() { Query q; q.limit = std::numeric_limits<qsizetype>::max(); return q; }
QStandardItem *cell(const QString &text) { auto *item = new QStandardItem(text); item->setEditable(false); return item; }
QString pickElement(QWidget *parent, const Projection &p, const QString &title, bool root = false) {
    QStringList labels, ids;
    if (root) { labels << QObject::tr("(Model root)"); ids << QString{}; }
    for (const auto &e : p.elements) { labels << e.name + " — " + e.kind + " [" + e.id.value + "]"; ids << e.id.value; }
    if (labels.isEmpty()) return {};
    bool ok = false;
    const auto chosen = QInputDialog::getItem(parent, title, QObject::tr("Element"), labels, 0, false, &ok);
    return ok ? ids.value(labels.indexOf(chosen)) : QString{};
}
std::optional<Geometry> geometryDialog(QWidget *parent, Geometry value) {
    QDialog dialog(parent); dialog.setWindowTitle(QObject::tr("Presentation geometry (center coordinates)"));
    QFormLayout form(&dialog); QVector<QDoubleSpinBox *> fields;
    const QStringList labels{QObject::tr("Center X"), QObject::tr("Center Y"), QObject::tr("Width"), QObject::tr("Height")};
    const double values[]{value.x,value.y,value.width,value.height};
    for (int i=0;i<4;++i) { auto *spin=new QDoubleSpinBox(&dialog);spin->setRange(i<2?-1e9:0.001,1e9);spin->setDecimals(3);spin->setValue(values[i]);form.addRow(labels[i],spin);fields.append(spin); }
    QDialogButtonBox buttons(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);form.addRow(&buttons);
    QObject::connect(&buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);QObject::connect(&buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if(dialog.exec()!=QDialog::Accepted)return std::nullopt;
    return Geometry{fields[0]->value(),fields[1]->value(),fields[2]->value(),fields[3]->value()};
}
}
MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), controller_(new WorkspaceController(this)) {
    setObjectName("MainWindow");resize(1200,800);setWindowTitle(tr("Rose Agent"));
    tabs_=new QTabWidget(this);tabs_->setObjectName("DiagramTabs");tabs_->setTabsClosable(true);setCentralWidget(tabs_);
    browser_=new QTreeView(this);browser_->setObjectName("ModelBrowser");treeModel_=new QStandardItemModel(this);browser_->setModel(treeModel_);browser_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    inspector_=new QTableView(this);inspector_->setObjectName("PropertyInspector");propertyModel_=new QStandardItemModel(this);inspector_->setModel(propertyModel_);inspector_->horizontalHeader()->setStretchLastSection(true);
    documentation_=new QPlainTextEdit(this);documentation_->setObjectName("ModelDocumentation");documentation_->setReadOnly(true);
    diagnostics_=new QPlainTextEdit(this);diagnostics_->setObjectName("WorkspaceDiagnostics");diagnostics_->setReadOnly(true);
    auto dock=[this](const QString &name,const QString &object,QWidget *widget,Qt::DockWidgetArea area){auto *d=new QDockWidget(name,this);d->setObjectName(object);d->setWidget(widget);addDockWidget(area,d);return d;};
    auto *modelDock=dock(tr("Model"),"ModelDock",browser_,Qt::LeftDockWidgetArea);
    auto *propertyDock=dock(tr("Properties"),"PropertiesDock",inspector_,Qt::RightDockWidgetArea);
    auto *docDock=dock(tr("Documentation"),"DocumentationDock",documentation_,Qt::BottomDockWidgetArea);
    auto *diagnosticDock=dock(tr("Diagnostics"),"DiagnosticsDock",diagnostics_,Qt::BottomDockWidgetArea);tabifyDockWidget(docDock,diagnosticDock);docDock->raise();
    agentDock_=new rose::agent::AgentDock(controller_,this);addDockWidget(Qt::RightDockWidgetArea,agentDock_);
    connect(agentDock_,&agent::AgentDock::newModelRequested,this,&MainWindow::newModel);
    connect(agentDock_,&agent::AgentDock::openModelRequested,this,&MainWindow::chooseOpen);
    connect(agentDock_,&agent::AgentDock::workflowStateChanged,this,&MainWindow::updateActions);
    buildActions();
    auto *viewMenu=menuBar()->findChild<QMenu *>("ViewMenu");for(auto *d:{modelDock,propertyDock,docDock,diagnosticDock})viewMenu->addAction(d->toggleViewAction());
    auto *agentMenu=menuBar()->addMenu(tr("&Agent"));agentMenu->addAction(agentDock_->toggleViewAction());
    connect(controller_,&WorkspaceController::completed,this,&MainWindow::receive);
    connect(browser_->selectionModel(),&QItemSelectionModel::selectionChanged,this,[this]{
        QVector<ElementId> ids;for(const auto &index:browser_->selectionModel()->selectedRows())if(auto id=treeObject(index))if(auto *element=std::get_if<ElementId>(&*id))ids.append(*element);
        agentDock_->setSelectedElements(std::move(ids));
    });
    connect(browser_->selectionModel(),&QItemSelectionModel::currentChanged,this,[this](const QModelIndex &index){if(auto id=treeObject(index))inspectObject(*id);});
    connect(browser_,&QTreeView::doubleClicked,this,[this](const QModelIndex &index){if(auto id=treeObject(index))if(auto *diagram=std::get_if<DiagramId>(&*id))openDiagram(*diagram);});
    connect(inspector_,&QTableView::doubleClicked,this,&MainWindow::editProperty);
    connect(tabs_,&QTabWidget::tabCloseRequested,this,[this](int index){auto *view=qobject_cast<DiagramView *>(tabs_->widget(index));if(view){for(auto it=diagrams_.begin();it!=diagrams_.end();){if(it.value()==view)it=diagrams_.erase(it);else ++it;}}tabs_->removeTab(index);delete view;});
    connect(tabs_,&QTabWidget::currentChanged,this,[this]{updateActions();});
    QSettings settings;restoreGeometry(settings.value("editor/geometry").toByteArray());
    if(settings.contains("editor/docks"))restoreState(settings.value("editor/docks").toByteArray());
    else{
        tabifyDockWidget(propertyDock,agentDock_);
        resizeDocks({modelDock,agentDock_},{180,440},Qt::Horizontal);
        resizeDocks({docDock},{120},Qt::Vertical);
    }
    agentDock_->show();agentDock_->raise();
    updateRecent({});updateActions();statusBar()->showMessage(tr("Ready"));
}
void MainWindow::buildActions() {
    auto *file=menuBar()->addMenu(tr("&File"));auto *edit=menuBar()->addMenu(tr("&Edit"));auto *view=menuBar()->addMenu(tr("&View"));view->setObjectName("ViewMenu");auto *diagram=menuBar()->addMenu(tr("&Diagram"));auto *model=menuBar()->addMenu(tr("&Model"));auto *tools=menuBar()->addMenu(tr("&Tools"));auto *help=menuBar()->addMenu(tr("&Help"));
    auto *toolbar=addToolBar(tr("Main"));toolbar->setObjectName("MainToolbar");
    auto action=[this](QMenu *menu,const QString &title,const QKeySequence &shortcut,auto operation){auto *a=menu->addAction(title);if(!shortcut.isEmpty())a->setShortcut(shortcut);connect(a,&QAction::triggered,this,operation);return a;};
    auto *newAction=action(file,tr("&New…"),QKeySequence::New,[this]{newModel();});newAction->setObjectName("NewModelAction");toolbar->addAction(newAction);
    auto *openAction=action(file,tr("&Open…"),QKeySequence::Open,[this]{chooseOpen();});openAction->setObjectName("OpenModelAction");toolbar->addAction(openAction);
    saveAction_=action(file,tr("&Save"),QKeySequence::Save,[this]{save();});saveAction_->setObjectName("SaveModelAction");toolbar->addAction(saveAction_);
    saveAsAction_=action(file,tr("Save &As…"),QKeySequence::SaveAs,[this]{save(true);});saveAsAction_->setObjectName("SaveModelAsAction");recentMenu_=file->addMenu(tr("Recent files"));
    action(file,tr("&Close model"),QKeySequence::Close,[this]{afterDirty([this]{track(controller_->close(),[](const auto &){});});});
    file->addSeparator();action(file,tr("&Print…"),QKeySequence::Print,[this]{if(auto *v=activeView())printDiagram(v->scene(),this,false);});action(file,tr("Print pre&view…"),{},[this]{if(auto *v=activeView())printDiagram(v->scene(),this,true);});
    action(file,tr("&Export diagram…"),{},[this]{if(auto *v=activeView()){auto path=QFileDialog::getSaveFileName(this,tr("Export diagram"),{},tr("PDF (*.pdf);;SVG (*.svg);;PNG (*.png)"));if(!path.isEmpty()){QString error;if(!exportDiagram(v->scene(),path,&error))QMessageBox::warning(this,tr("Export failed"),error);}}});
    file->addSeparator();action(file,tr("&Quit"),QKeySequence::Quit,[this]{close();});
    undoAction_=action(edit,tr("&Undo"),QKeySequence::Undo,[this]{track(controller_->undo(),[](const auto &){});});redoAction_=action(edit,tr("&Redo"),QKeySequence::Redo,[this]{track(controller_->redo(),[](const auto &){});});toolbar->addSeparator();toolbar->addAction(undoAction_);toolbar->addAction(redoAction_);
    auto modelAction=[&](QMenu *menu,const QString &title,auto operation){auto *a=action(menu,title,{},operation);modelActions_.append(a);return a;};
    modelAction(model,tr("Create &element…"),[this]{createElement();});modelAction(model,tr("&Rename…"),[this]{renameElement();})->setShortcut(Qt::Key_F2);
    modelAction(model,tr("Change &owner…"),[this]{reparentElement(false);});modelAction(model,tr("&Copy subtree…"),[this]{reparentElement(true);});modelAction(model,tr("&Delete element and dependents…"),[this]{deleteElement();});
    modelAction(model,tr("Create &relation…"),[this]{createRelation();});modelAction(model,tr("Reconnect relation…"),[this]{reconnectRelation();});
    modelAction(model,tr("Edit documentation…"),[this]{if(!inspected_)return;bool ok=false;auto text=QInputDialog::getMultiLineText(this,tr("Documentation"),tr("Plain text"),documentation_->toPlainText(),&ok);if(ok)submit({{SetProperty{*inspected_,"documentation",text}}});});
    modelAction(diagram,tr("&New diagram…"),[this]{createDiagram();});modelAction(diagram,tr("&Add existing element…"),[this]{addPresentation();});modelAction(diagram,tr("&Hide selected presentations"),[this]{removePresentation();});modelAction(diagram,tr("&Geometry…"),[this]{editGeometry();});modelAction(diagram,tr("Edit &route…"),[this]{editRoute();});
    modelAction(diagram,tr("Align centers &horizontally"),[this]{align(true);});modelAction(diagram,tr("Align centers &vertically"),[this]{align(false);});
    action(diagram,tr("Zoom &in"),QKeySequence::ZoomIn,[this]{if(auto *v=activeView())v->zoomIn();});action(diagram,tr("Zoom &out"),QKeySequence::ZoomOut,[this]{if(auto *v=activeView())v->zoomOut();});action(diagram,tr("&Fit diagram"),{},[this]{if(auto *v=activeView())v->fitDiagram();});
    action(tools,tr("Workspace &access…"),{},[this]{configureAccess();});
    action(help,tr("&About"),{},[this]{QMessageBox::about(this,tr("Rose Agent"),tr("Software-engineering AI assistance for native UML models: prompt or image → inspect → exact transaction review → apply in memory → separate native .mdl save. Manual edits and saving work offline. Imported unsupported content is preserved, not silently reinterpreted. New models and native diagram writing support only the implemented Petal profiles and diagram families; this is not a full Rational Rose replacement. Compatibility must be checked in the target Rose version."));});
}
void MainWindow::track(quint64 request,Callback callback){pending_.insert(request,std::move(callback));}
void MainWindow::receive(quint64 request,const WorkspaceReply &reply){
    const bool tracked=pending_.contains(request);
    auto callback=pending_.take(request);
    if(auto *error=std::get_if<WorkspaceError>(&reply)){if(tracked){fileBusy_=false;failure(*error);if(controller_->hasWorkspace())refresh();}updateActions();return;}
    if(std::holds_alternative<Closed>(reply)){tree_={};dirty_=false;treeModel_->clear();propertyModel_->clear();documentation_->clear();diagrams_.clear();while(tabs_->count()){auto *widget=tabs_->widget(0);tabs_->removeTab(0);delete widget;}inspected_.reset();}
    if(callback)callback(reply);
    if(std::holds_alternative<Applied>(reply)){dirty_=true;refresh();}
    // Rejected does not close or clear the workspace. Agent replies also arrive here.
    if(std::holds_alternative<SaveReceipt>(reply)){dirty_=false;refresh();}
    if(std::holds_alternative<AccessGranted>(reply))refresh();
    updateActions();
}
void MainWindow::refresh(){if(!controller_->hasWorkspace())return;track(controller_->inspect(allTree()),[this](const auto &reply){if(auto *projection=std::get_if<Projection>(&reply)){populate(*projection);const auto ids=diagrams_.keys();for(const auto &id:ids)refreshDiagram(id);if(inspected_)inspectObject(*inspected_);}});}
void MainWindow::populate(const Projection &projection){
    DiagramId addedDiagram;
    if(!tree_.diagrams.isEmpty())for(const auto &diagram:projection.diagrams)
        if(diagram.kind=="ClassDiagram"&&std::none_of(tree_.diagrams.cbegin(),tree_.diagrams.cend(),[&](const auto &old){return old.id==diagram.id;})){addedDiagram=diagram.id;break;}
    auto selected=treeObject(browser_->currentIndex());tree_=projection;dirty_=projection.dirty;treeModel_->clear();treeModel_->setHorizontalHeaderLabels({tr("Name"),tr("Kind")});
    QHash<ElementId,QStandardItem *> nodes;
    for(const auto &e:projection.elements){auto *item=cell(e.name);item->setData(e.id.value,IdRole);item->setData("element",TypeRole);item->setToolTip(e.id.value);nodes.insert(e.id,item);}
    for(const auto &e:projection.elements){auto *parent=nodes.value(e.owner,treeModel_->invisibleRootItem()); // Avoid malformed cycles in UI ownership.
        QSet<ElementId> ancestors;auto owner=e.owner;bool cycle=false;while(!owner.isEmpty()){if(owner==e.id||ancestors.contains(owner)){cycle=true;break;}ancestors.insert(owner);auto found=std::find_if(projection.elements.cbegin(),projection.elements.cend(),[&](const auto &r){return r.id==owner;});if(found==projection.elements.cend())break;owner=found->owner;}
        if(cycle)parent=treeModel_->invisibleRootItem();parent->appendRow({nodes.value(e.id),cell(e.kind)});}
    for(const auto &r:projection.relations){auto *item=cell(r.name.isEmpty()?r.kind:r.name);item->setData(r.id.value,IdRole);item->setData("relation",TypeRole);nodes.value(r.owner,treeModel_->invisibleRootItem())->appendRow({item,cell(r.kind)});}
    for(const auto &d:projection.diagrams){auto *item=cell(d.name);item->setData(d.id.value,IdRole);item->setData("diagram",TypeRole);nodes.value(d.owner,treeModel_->invisibleRootItem())->appendRow({item,cell(d.kind)});}
    for(const auto &u:projection.units)if(!u.resolved){auto *item=cell(tr("Unresolved unit: %1").arg(u.path));item->setToolTip(tr("No synthetic package was created"));treeModel_->appendRow({item,cell(tr("Missing unit"))});}
    if(selected){std::function<void(const QModelIndex &)> restore=[&](const QModelIndex &parent){for(int row=0;row<treeModel_->rowCount(parent);++row){auto index=treeModel_->index(row,0,parent);if(treeObject(index)==selected)browser_->setCurrentIndex(index);restore(index);}};restore({});}
    for(const auto &d:projection.diagnostics)diagnostic(d);browser_->resizeColumnToContents(0);updateActions();
    if(!addedDiagram.isEmpty())openDiagram(addedDiagram);
    else if(tabs_->count()==0&&!projection.diagrams.isEmpty()){
        auto diagram=std::find_if(projection.diagrams.cbegin(),projection.diagrams.cend(),[](const auto &d){return d.kind=="ClassDiagram"&&!d.presentations.isEmpty();});
        if(diagram==projection.diagrams.cend())diagram=std::find_if(projection.diagrams.cbegin(),projection.diagrams.cend(),[](const auto &d){return d.kind=="ClassDiagram";});
        openDiagram(diagram==projection.diagrams.cend()?projection.diagrams.first().id:diagram->id);
    }
}
void MainWindow::refreshDiagram(const DiagramId &id){Query q;q.kind=Query::Kind::DiagramById;q.diagram=id;q.limit=std::numeric_limits<qsizetype>::max();track(controller_->inspect(q),[this,id](const auto &reply){if(auto *p=std::get_if<Projection>(&reply)){auto view=diagrams_.value(id);if(!view)return;auto selected=view->diagramScene()->selectedPresentations();const bool firstContent=view->diagramScene()->projection().presentations.isEmpty()&&!p->presentations.isEmpty();view->diagramScene()->setProjection(*p);for(const auto &pid:selected)if(auto *item=view->diagramScene()->presentationItem(pid))item->setSelected(true);if(firstContent)view->fitDiagram();for(const auto &d:p->diagrams)if(d.id==id)tabs_->setTabText(tabs_->indexOf(view),d.name);}});}
void MainWindow::openDiagram(const DiagramId &id){if(auto view=diagrams_.value(id)){tabs_->setCurrentWidget(view);refreshDiagram(id);return;}auto *scene=new DiagramScene(this);auto *view=new DiagramView(scene,tabs_);view->setObjectName("DiagramView_"+id.value);scene->setParent(view);diagrams_.insert(id,view);QString title=id.value;for(const auto &d:tree_.diagrams)if(d.id==id)title=d.name;tabs_->addTab(view,title);tabs_->setCurrentWidget(view);
    connect(scene,&DiagramScene::diagnostic,this,&MainWindow::diagnostic);connect(view,&DiagramView::commandsRequested,this,[this](QVector<Command> commands){submit(std::move(commands));});connect(view,&DiagramView::removeRequested,this,&MainWindow::removePresentation);
    connect(scene,&QGraphicsScene::selectionChanged,this,[this,view]{
        if(view!=activeView())return;auto ids=view->diagramScene()->selectedPresentations();if(!ids.isEmpty())inspectObject(ObjectId{ids.first()});
        QVector<ElementId> selected;for(const auto &p:view->diagramScene()->projection().presentations)if(ids.contains(p.id)&&!p.element.isEmpty()&&!selected.contains(p.element))selected.append(p.element);
        agentDock_->setSelectedElements(std::move(selected));
    });refreshDiagram(id);
}
std::optional<ObjectId> MainWindow::treeObject(const QModelIndex &index)const{auto first=index.siblingAtColumn(0);auto id=first.data(IdRole).toString();auto type=first.data(TypeRole).toString();if(id.isEmpty())return {};if(type=="element")return ObjectId{ElementId{id}};if(type=="relation")return ObjectId{RelationId{id}};if(type=="diagram")return ObjectId{DiagramId{id}};return {};}
ElementId MainWindow::selectedElement()const{if(auto id=treeObject(browser_->currentIndex()))if(auto *element=std::get_if<ElementId>(&*id))return *element;return {};}
DiagramView *MainWindow::activeView()const{return qobject_cast<DiagramView *>(tabs_->currentWidget());}
void MainWindow::inspectObject(const ObjectId &id){
    inspected_=id;propertyModel_->clear();propertyModel_->setHorizontalHeaderLabels({tr("Property"),tr("Value"),tr("Type")});documentation_->clear();
    auto add=[this](const QString &key,const QString &value,const QString &type,bool editable=false){auto *item=cell(key);item->setData(editable,IdRole);propertyModel_->appendRow({item,cell(value),cell(type)});};
    add("id",idText(id),"identity");QMap<QString,QString> properties,types;bool readOnly=true,found=false;
    auto record=[&](const auto &r){properties=r.properties;types=r.propertyTypes;readOnly=r.readOnly;found=true;};
    const auto *p=&tree_;if(std::holds_alternative<PresentationId>(id)&&activeView())p=&activeView()->diagramScene()->projection();
    if(auto *e=std::get_if<ElementId>(&id))for(const auto &r:p->elements)if(r.id==*e){record(r);add("name",r.name,"name",!r.readOnly);add("kind",r.kind,"type");add("owner",r.owner.value,"identity");}
    if(auto *e=std::get_if<RelationId>(&id))for(const auto &r:p->relations)if(r.id==*e){record(r);add("name",r.name,"string");add("kind",r.kind,"type");QStringList ends;for(const auto &end:r.endpoints)ends<<end.value;add("endpoints",ends.join(", "),"identities");}
    if(auto *e=std::get_if<PresentationId>(&id))for(const auto &r:p->presentations)if(r.id==*e){record(r);add("kind",r.kind,"type");add("center",QString::number(r.geometry.x)+", "+QString::number(r.geometry.y),"geometry");add("size",QString::number(r.geometry.width)+", "+QString::number(r.geometry.height),"geometry");}
    if(auto *e=std::get_if<DiagramId>(&id))for(const auto &r:p->diagrams)if(r.id==*e){add("name",r.name,"string");add("kind",r.kind,"type");found=true;}
    if(!found){add("state",tr("Object is unavailable in the current projection"),"diagnostic");return;}
    for(auto it=properties.cbegin();it!=properties.cend();++it){const auto type=types.value(it.key());add(it.key(),it.value(),type,!readOnly&&!type.isEmpty()&&type!="opaque"&&type!="identity");if(it.key().compare("documentation",Qt::CaseInsensitive)==0)documentation_->setPlainText(it.value());}
}
void MainWindow::submit(QVector<Command> commands){if(commands.isEmpty()||!controller_->hasWorkspace()||fileBusy_||agentDock_->busy())return;const auto generation=controller_->sessionGeneration();track(controller_->propose(controller_->revision(),commands),[this,generation](const auto &reply){auto *proposal=std::get_if<Proposal>(&reply);if(!proposal)return;if(generation!=controller_->sessionGeneration())return;
    if(!review(*proposal)){track(controller_->reject(proposal->id),[this](const auto &){refresh();});return;}
    if(proposal->baseRevision!=controller_->revision()){track(controller_->reject(proposal->id),[](const auto &){});failure({ErrorCode::StaleApproval,tr("Model changed during review. Review a fresh proposal.")});refresh();return;}
    track(controller_->apply({proposal->id,proposal->baseRevision,proposal->digest}),[](const auto &){});
});}
bool MainWindow::review(const Proposal &proposal){
    QDialog dialog(this);dialog.setObjectName("ManualProposalReview");dialog.setWindowTitle(tr("Review model transaction"));QVBoxLayout layout(&dialog);
    QLabel heading(tr("Apply only this exact transaction in memory? Saving remains separate.\nProposal: %1\nRevision: %2\nDigest: %3").arg(proposal.id.value).arg(proposal.baseRevision).arg(QString::fromLatin1(proposal.digest.toHex())),&dialog);
    heading.setObjectName("ManualProposalTuple");heading.setWordWrap(true);heading.setTextInteractionFlags(Qt::TextSelectableByMouse);layout.addWidget(&heading);
    QTableView view(&dialog);view.setObjectName("ManualProposalDiff");QStandardItemModel model(&dialog);model.setHorizontalHeaderLabels({tr("Change"),tr("Object"),tr("Field"),tr("Before"),tr("After")});
    for(const auto &d:proposal.diff)model.appendRow({cell(d.kind),cell(d.objectId),cell(d.field),cell(d.before),cell(d.after)});
    view.setModel(&model);view.horizontalHeader()->setStretchLastSection(true);view.resizeColumnsToContents();view.resizeRowsToContents();layout.addWidget(&view);
    QPlainTextEdit warnings(&dialog);warnings.setObjectName("ManualProposalDiagnostics");warnings.setReadOnly(true);
    for(const auto &d:proposal.diagnostics)warnings.appendPlainText((d.severity==Severity::Error?tr("Error"):d.severity==Severity::Warning?tr("Warning"):tr("Information"))+" — "+d.code+": "+d.message+"\nobject="+d.objectId+" file="+d.file+" span="+QString::number(d.span.offset)+":"+QString::number(d.span.length));
    for(auto it=proposal.newIds.cbegin();it!=proposal.newIds.cend();++it)warnings.appendPlainText(it.key()+" → "+it.value());
    if(proposal.diagnostics.isEmpty()&&proposal.newIds.isEmpty())warnings.setPlainText(tr("No proposal diagnostics."));
    layout.addWidget(&warnings);QDialogButtonBox buttons(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
    buttons.button(QDialogButtonBox::Ok)->setText(tr("Apply transaction"));buttons.button(QDialogButtonBox::Ok)->setObjectName("ManualApply");
    buttons.button(QDialogButtonBox::Cancel)->setText(tr("Reject transaction"));buttons.button(QDialogButtonBox::Cancel)->setObjectName("ManualReject");
    layout.addWidget(&buttons);connect(&buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(&buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    dialog.resize(900,540);return dialog.exec()==QDialog::Accepted;
}
void MainWindow::diagnostic(const Diagnostic &d){diagnostics_->appendPlainText(d.code+": "+d.message+(d.file.isEmpty()?QString{}:" ["+d.file+"]")+(d.objectId.isEmpty()?QString{}:" ("+d.objectId+")"));}
void MainWindow::failure(const WorkspaceError &e){auto text=e.message;if(!e.file.isEmpty())text+=tr("\n%1:%2:%3").arg(e.file).arg(e.line).arg(e.column);diagnostics_->appendPlainText(text);QMessageBox::warning(this,tr("Workspace operation failed"),text);}
void MainWindow::afterDirty(std::function<void()> after){if(fileBusy_||!pending_.isEmpty()||agentDock_->busy()){QMessageBox::information(this,tr("Operation in progress"),tr("Wait for pending operations and apply, reject or cancel the agent review before replacing or closing this workspace."));return;}if(!dirty_){after();return;}auto answer=QMessageBox::warning(this,tr("Unsaved model"),tr("Save changes before continuing?"),QMessageBox::Save|QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Save);if(answer==QMessageBox::Save)save(false,std::move(after));else if(answer==QMessageBox::Discard)after();}
void MainWindow::save(bool saveAs,std::function<void()> after){if(!controller_->hasWorkspace()||fileBusy_)return;SaveMode mode;if(saveAs){mode.destination=QFileDialog::getSaveFileName(this,tr("Save model as"),tree_.path,tr("Rose models (*.mdl)"));if(mode.destination.isEmpty())return;auto directory=QFileInfo(mode.destination).absolutePath();if(!access_.allowedDirectories.contains(directory)){if(QMessageBox::question(this,tr("Allow destination access"),tr("Allow writes to %1 for this workspace?").arg(directory))!=QMessageBox::Yes)return;fileBusy_=true;track(controller_->grantDirectory(directory),[this,directory,mode,after=std::move(after)](const auto &reply)mutable{if(!std::holds_alternative<AccessGranted>(reply))return;access_.allowedDirectories.append(directory);track(controller_->save(mode),[this,after=std::move(after)](const auto &r){fileBusy_=false;if(auto *receipt=std::get_if<SaveReceipt>(&r)){dirty_=false;updateRecent(receipt->path);if(after)after();}});});return;}}
    fileBusy_=true;updateActions();track(controller_->save(mode),[this,after=std::move(after)](const auto &reply){fileBusy_=false;if(auto *receipt=std::get_if<SaveReceipt>(&reply)){dirty_=false;updateRecent(receipt->path);if(after)after();}});
}
bool MainWindow::choosePolicy(const QString &path,AccessPolicy &policy,bool creating,RoseProfile *profile){
    QDialog dialog(this);dialog.setObjectName("NativeFilePolicyDialog");dialog.setWindowTitle(tr("Native file access and encoding"));QFormLayout form(&dialog);
    QLabel file(path,&dialog);file.setTextInteractionFlags(Qt::TextSelectableByMouse);form.addRow(tr("File"),&file);
    QPlainTextEdit directories(QFileInfo(path).absolutePath(),&dialog);directories.setObjectName("AllowedDirectories");directories.setMaximumHeight(80);form.addRow(tr("Allowed directories (one per line)"),&directories);
    QComboBox encoding(&dialog);encoding.setObjectName("SourceEncoding");encoding.setEditable(true);encoding.addItems({"ASCII","UTF-8","Windows-1252","ISO-8859-1"});encoding.setCurrentText(QString::fromLatin1(policy.sourceEncoding));form.addRow(tr("Explicit source encoding"),&encoding);
    QComboBox version(&dialog);version.setObjectName("PetalProfile");version.addItem(tr("Petal 44 — Rose2000e"),44);version.addItem(tr("Petal 50 — newer Rose"),50);
    if(creating)form.addRow(tr("New native profile"),&version);else version.hide();
    QComboBox access(&dialog);access.setObjectName("WorkspaceAccess");access.addItems({tr("Read/write"),tr("Read only")});access.setCurrentIndex(policy.writable?0:1);form.addRow(tr("Access"),&access);
    QPlainTextEdit variables(&dialog);variables.setObjectName("PathVariables");variables.setMaximumHeight(100);for(auto it=policy.pathVariables.cbegin();it!=policy.pathVariables.cend();++it)variables.appendPlainText(it.key()+"="+it.value());form.addRow(tr("Path variables (NAME=directory)"),&variables);
    QLabel limits(creating?tr("New creates an in-memory native model. Nothing is written until Save. Authored output: classes, packages, attributes, operations and parameters; class diagrams with class/package views and association/inheritance connectors. Other imported families remain preserved, not authored. Petal 44/50 are explicit output profiles, not verified in your Rational Rose installation."):tr("Unsupported profiles/codecs are diagnosed, never guessed. Unsupported imported content and diagram families are preserved, but may not be rendered or editable. Missing controlled units are reported. New output is limited to supported class-model semantics and class-diagram views, association and inheritance connectors."),&dialog);
    limits.setWordWrap(true);form.addRow(&limits);
    QDialogButtonBox buttons(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);buttons.button(QDialogButtonBox::Ok)->setObjectName("AcceptNativeFilePolicy");buttons.button(QDialogButtonBox::Cancel)->setObjectName("CancelNativeFilePolicy");form.addRow(&buttons);
    connect(&buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(&buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    dialog.resize(660,dialog.sizeHint().height());if(dialog.exec()!=QDialog::Accepted)return false;
    policy.allowedDirectories=directories.toPlainText().split('\n',Qt::SkipEmptyParts);for(auto &directory:policy.allowedDirectories)directory=directory.trimmed();
    policy.sourceEncoding=encoding.currentText().toLatin1();policy.writable=access.currentIndex()==0;policy.pathVariables.clear();
    for(const auto &line:variables.toPlainText().split('\n',Qt::SkipEmptyParts)){auto at=line.indexOf('=');if(at<=0){QMessageBox::warning(this,tr("Invalid path variable"),tr("Use NAME=directory for each mapping."));return false;}policy.pathVariables.insert(line.left(at).trimmed(),line.mid(at+1).trimmed());}
    if(profile){profile->petalVersion=version.currentData().toInt();profile->encoding=policy.sourceEncoding;}
    return true;
}
void MainWindow::openPath(const QString &path){afterDirty([this,path]{AccessPolicy policy=access_;if(!choosePolicy(path,policy,false))return;fileBusy_=true;updateActions();track(controller_->open(path,policy),[this,policy](const auto &reply){fileBusy_=false;if(auto *projection=std::get_if<Projection>(&reply)){access_=policy;diagrams_.clear();while(tabs_->count()){auto *w=tabs_->widget(0);tabs_->removeTab(0);delete w;}inspected_.reset();propertyModel_->clear();documentation_->clear();populate(*projection);updateRecent(projection->path);}});});}
void MainWindow::chooseOpen(){auto path=QFileDialog::getOpenFileName(this,tr("Open Rose model"),{},tr("Rose models (*.mdl);;All files (*)"));if(!path.isEmpty())openPath(path);}
void MainWindow::newModel(){afterDirty([this]{auto path=QFileDialog::getSaveFileName(this,tr("New native model destination (written only on Save)"),{},tr("Rose models (*.mdl)"));if(path.isEmpty())return;if(QFileInfo(path).suffix().isEmpty())path+=".mdl";AccessPolicy policy;RoseProfile profile;if(!choosePolicy(path,policy,true,&profile))return;fileBusy_=true;updateActions();track(controller_->create(path,profile,policy),[this,policy](const auto &reply){fileBusy_=false;if(auto *p=std::get_if<Projection>(&reply)){access_=policy;diagrams_.clear();while(tabs_->count()){auto *w=tabs_->widget(0);tabs_->removeTab(0);delete w;}inspected_.reset();propertyModel_->clear();documentation_->clear();populate(*p);}});});}
void MainWindow::configureAccess(){if(!controller_->hasWorkspace())return;auto directory=QFileDialog::getExistingDirectory(this,tr("Grant workspace directory access"));if(directory.isEmpty())return;if(QMessageBox::question(this,tr("Grant access"),tr("Allow this workspace to read and write under %1? This does not save any file.").arg(directory))!=QMessageBox::Yes)return;track(controller_->grantDirectory(directory),[this,directory](const auto &reply){if(std::holds_alternative<AccessGranted>(reply))access_.allowedDirectories.append(directory);});}
void MainWindow::createElement(){bool ok=false;auto kind=QInputDialog::getItem(this,tr("Create UML element"),tr("Supported native class-model kind"),{"Class","Class_Category","ClassAttribute","Operation","Parameter"},0,false,&ok);if(!ok||kind.isEmpty())return;auto name=QInputDialog::getText(this,tr("Create element"),tr("Name"),QLineEdit::Normal,{},&ok);if(ok)submit({{CreateElement{fresh(),kind,name,selectedElement(),{}}}});}
void MainWindow::renameElement(){auto id=selectedElement();if(id.isEmpty())return;QString current;for(const auto &e:tree_.elements)if(e.id==id)current=e.name;bool ok=false;auto name=QInputDialog::getText(this,tr("Rename element"),tr("Name"),QLineEdit::Normal,current,&ok);if(ok)submit({{RenameElement{id,name}}});}
void MainWindow::reparentElement(bool copy){auto id=selectedElement();if(id.isEmpty())return;auto owner=pickElement(this,tree_,copy?tr("Copy subtree to owner"):tr("Move element to owner"));if(owner.isEmpty())return;if(copy)submit({{CopyElements{{id},ElementId{owner},{}}}});else submit({{SetOwner{id,ElementId{owner}}}});}
void MainWindow::deleteElement(){auto id=selectedElement();if(id.isEmpty())return;Query query;query.kind=Query::Kind::Neighborhood;query.focus=id;query.depth=std::numeric_limits<int>::max();query.limit=std::numeric_limits<qsizetype>::max();track(controller_->inspect(query),[this,id](const auto &reply){auto *p=std::get_if<Projection>(&reply);if(!p)return;QStringList dependents;QSet<ElementId> descendants{id};bool changed=true;while(changed){changed=false;for(const auto &e:p->elements)if(descendants.contains(e.owner)&&!descendants.contains(e.id)){descendants.insert(e.id);changed=true;}}for(const auto &e:descendants)if(e!=id)dependents<<e.value;for(const auto &r:p->relations){bool impacted=descendants.contains(r.owner);for(const auto &end:r.endpoints)impacted|=descendants.contains(end);if(impacted)dependents<<r.id.value;}for(const auto &d:p->diagrams)if(descendants.contains(d.owner))dependents<<d.id.value;for(const auto &v:p->presentations)if(descendants.contains(v.element)||dependents.contains(v.relation.value)||dependents.contains(v.diagram.value))dependents<<v.id.value;dependents.removeDuplicates();submit({{DeleteElement{id,dependents}}});});}
void MainWindow::createDiagram(){bool ok=false;auto name=QInputDialog::getText(this,tr("Create native class diagram"),tr("Name (other imported diagram families remain preserved)"),QLineEdit::Normal,{},&ok);if(ok)submit({{CreateDiagram{fresh(),"ClassDiagram",name,selectedElement(),{}}}});}
void MainWindow::addPresentation(){auto *view=activeView();if(!view)return;DiagramId diagram;for(auto it=diagrams_.cbegin();it!=diagrams_.cend();++it)if(it.value()==view)diagram=it.key();auto id=pickElement(this,tree_,tr("Add existing element"));if(id.isEmpty())return;auto center=view->mapToScene(view->viewport()->rect().center());auto geometry=geometryDialog(this,{center.x(),center.y(),160,100});if(geometry)submit({{AddPresentation{fresh(),diagram,ObjectId{ElementId{id}},*geometry}}});}
void MainWindow::removePresentation(){auto *view=activeView();if(!view)return;QVector<Command> commands;for(const auto &id:view->diagramScene()->selectedPresentations())commands.append({RemovePresentation{id}});submit(std::move(commands));}
void MainWindow::createRelation(){bool ok=false;auto kind=QInputDialog::getItem(this,tr("Create relation"),tr("Supported native class-model relation"),{"Association","Inheritance_Relationship"},0,false,&ok);if(!ok)return;auto from=pickElement(this,tree_,tr("Relation source"));if(from.isEmpty())return;auto to=pickElement(this,tree_,tr("Relation target"));if(to.isEmpty())return;auto name=QInputDialog::getText(this,tr("Create relation"),tr("Name"),QLineEdit::Normal,{},&ok);if(!ok)return;auto client=fresh();QVector<Command> commands{{CreateRelation{client,kind,name,selectedElement(),{{from},{to}}, {}}}};if(auto *view=activeView()){DiagramId diagram;for(auto it=diagrams_.cbegin();it!=diagrams_.cend();++it)if(it.value()==view)diagram=it.key();commands.append({AddPresentation{fresh(),diagram,ObjectId{RelationId{client}},{0,0,160,100}}});}submit(std::move(commands));}
void MainWindow::reconnectRelation(){if(!inspected_)return;RelationId id;if(auto *r=std::get_if<RelationId>(&*inspected_))id=*r;else if(auto *v=std::get_if<PresentationId>(&*inspected_)){if(auto *view=activeView())for(const auto &record:view->diagramScene()->projection().presentations)if(record.id==*v)id=record.relation;}if(id.isEmpty())return;auto from=pickElement(this,tree_,tr("New relation source"));if(from.isEmpty())return;auto to=pickElement(this,tree_,tr("New relation target"));if(!to.isEmpty())submit({{ReconnectRelation{id,{{from},{to}}}}});}
void MainWindow::editGeometry(){auto *view=activeView();if(!view)return;auto ids=view->diagramScene()->selectedPresentations();if(ids.size()!=1)return;for(const auto &p:view->diagramScene()->projection().presentations)if(p.id==ids.first()){auto g=geometryDialog(this,p.geometry);if(g)submit({{SetGeometry{p.id,*g}}});}}
void MainWindow::editRoute(){auto *view=activeView();if(!view)return;auto ids=view->diagramScene()->selectedPresentations();if(ids.size()!=1)return;for(const auto &p:view->diagramScene()->projection().presentations)if(p.id==ids.first()){QStringList lines;for(const auto &point:p.route)lines<<QString::number(point.x,'g',16)+", "+QString::number(point.y,'g',16);bool ok=false;auto text=QInputDialog::getMultiLineText(this,tr("Connector route"),tr("World X,Y per line; empty restores endpoint routing"),lines.join('\n'),&ok);if(!ok)return;QVector<WorldPoint> points;for(const auto &line:text.split('\n',Qt::SkipEmptyParts)){auto parts=line.split(',');bool xok=false,yok=false;auto x=parts.value(0).trimmed().toDouble(&xok),y=parts.value(1).trimmed().toDouble(&yok);if(parts.size()!=2||!xok||!yok||!std::isfinite(x)||!std::isfinite(y)){QMessageBox::warning(this,tr("Invalid route"),tr("Each line must contain two finite coordinates."));return;}points.append({x,y});}submit({{SetRoute{p.id,points}}});}}
void MainWindow::align(bool horizontal){auto *view=activeView();if(!view)return;auto ids=view->diagramScene()->selectedPresentations();if(ids.size()<2)return;QVector<PresentationRecord> records;double center=0;for(const auto &p:view->diagramScene()->projection().presentations)if(ids.contains(p.id)&&p.relation.isEmpty()){records.append(p);center+=horizontal?p.geometry.y:p.geometry.x;}if(records.size()<2)return;center/=records.size();QVector<Command> commands;for(const auto &p:records){auto g=p.geometry;if(horizontal)g.y=center;else g.x=center;commands.append({SetGeometry{p.id,g}});}submit(std::move(commands));}
void MainWindow::editProperty(const QModelIndex &index){if(!inspected_)return;auto key=propertyModel_->index(index.row(),0);if(!key.data(IdRole).toBool())return;auto type=propertyModel_->index(index.row(),2).data().toString();auto current=propertyModel_->index(index.row(),1).data().toString();bool ok=false;EncodedValue value;
    if(type=="name"){auto text=QInputDialog::getText(this,tr("Rename element"),key.data().toString(),QLineEdit::Normal,current,&ok);if(ok)if(auto *id=std::get_if<ElementId>(&*inspected_))submit({{RenameElement{*id,text}}});return;}
    if(type=="bool"||type=="boolean"){auto choice=QInputDialog::getItem(this,tr("Edit property"),key.data().toString(),{"FALSE","TRUE"},current.compare("TRUE",Qt::CaseInsensitive)==0?1:0,false,&ok);value=choice=="TRUE";}
    else if(type.startsWith("enum:")){auto choices=type.mid(5).split('|');auto choice=QInputDialog::getItem(this,tr("Edit property"),key.data().toString(),choices,std::max(0,int(choices.indexOf(current))),false,&ok);value=choice;}
    else {auto text=QInputDialog::getText(this,tr("Edit property"),key.data().toString(),QLineEdit::Normal,current,&ok);if(!ok)return;if(type=="integer"||type=="int"){auto number=text.toLongLong(&ok);value=number;}else if(type=="number"||type=="double"||type=="real"){auto number=text.toDouble(&ok);ok=ok&&std::isfinite(number);value=number;}else if(type=="reference"||type=="local-reference"){auto number=text.startsWith('@')?text.mid(1).toULongLong(&ok):text.toULongLong(&ok);value=LocalReference{number};}else value=text;}
    if(ok)submit({{SetProperty{*inspected_,key.data().toString(),value}}});else QMessageBox::warning(this,tr("Invalid property value"),tr("The value does not match its native property type."));
}
void MainWindow::updateActions(){
    const bool enabled=controller_->hasWorkspace()&&!fileBusy_&&!agentDock_->busy();
    saveAction_->setEnabled(enabled&&access_.writable);saveAsAction_->setEnabled(enabled&&access_.writable);
    undoAction_->setEnabled(enabled&&tree_.canUndo&&access_.writable);redoAction_->setEnabled(enabled&&tree_.canRedo&&access_.writable);
    for(auto *a:modelActions_)a->setEnabled(enabled&&access_.writable);
    for(auto view:diagrams_)if(view)view->setEnabled(enabled);
    agentDock_->setWorkspaceState(fileBusy_,access_.writable);
    setWindowTitle((tree_.path.isEmpty()?tr("No model"):QFileInfo(tree_.path).fileName())+(dirty_?" *":"")+tr(" — Rose Agent"));
    statusBar()->showMessage(fileBusy_?tr("File operation in progress…"):!controller_->hasWorkspace()?tr("New or Open a model to begin"):tr("Revision %1 — %2").arg(controller_->revision()).arg(dirty_?tr("Modified, not saved"):tr("Saved")));
}
void MainWindow::updateRecent(const QString &path){QSettings settings;auto files=settings.value("editor/recent").toStringList();if(!path.isEmpty()){files.removeAll(path);files.prepend(path);while(files.size()>10)files.removeLast();settings.setValue("editor/recent",files);}recentMenu_->clear();for(const auto &file:files){auto *action=recentMenu_->addAction(file);connect(action,&QAction::triggered,this,[this,file]{openPath(file);});}recentMenu_->setEnabled(!files.isEmpty());}
void MainWindow::closeEvent(QCloseEvent *event){if(closing_){event->accept();return;}event->ignore();afterDirty([this]{QSettings settings;settings.setValue("editor/geometry",saveGeometry());settings.setValue("editor/docks",saveState());closing_=true;close();});}
}
