#include "DiagramScene.h"
#include <QApplication>
#include <QGraphicsPathItem>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>
#include <cmath>
#include <algorithm>
namespace rose::desktop {
namespace {
// Keep native geometry unchanged; scale Qt text/strokes to Rose diagram units.
constexpr double nativeUnitsPerPixel = 3.125;
enum class Glyph { Actor, UseCase, Class, Object, Lifeline, State, Initial, Final, Decision, Fork, Activity, Component, Node, Package, Note, Swimlane, Unknown };
Glyph glyph(QString kind) {
    kind = kind.toLower();
    if (kind.endsWith("view")) kind.chop(4);
    if (kind == "actor") return Glyph::Actor;
    if (kind == "usecase" || kind == "use_case") return Glyph::UseCase;
    if (kind == "class") return Glyph::Class;
    if (kind == "object" || kind == "instance" || kind == "classinstance") return Glyph::Object;
    if (kind == "lifeline" || kind == "objecttime" || kind == "objectlifeline") return Glyph::Lifeline;
    if (kind == "state" || kind == "compositestate") return Glyph::State;
    if (kind == "startstate" || kind == "initialstate" || kind == "initial") return Glyph::Initial;
    if (kind == "endstate" || kind == "finalstate" || kind == "final") return Glyph::Final;
    if (kind == "decision" || kind == "junction" || kind == "branch") return Glyph::Decision;
    if (kind == "synchronization" || kind == "fork" || kind == "join") return Glyph::Fork;
    if (kind == "activity" || kind == "actionstate" || kind == "action") return Glyph::Activity;
    if (kind == "component" || kind == "module") return Glyph::Component;
    if (kind == "processor" || kind == "device" || kind == "node") return Glyph::Node;
    if (kind == "package" || kind == "category" || kind == "subsystem") return Glyph::Package;
    if (kind == "note" || kind == "annotation" || kind == "text") return Glyph::Note;
    if (kind == "swimlane" || kind == "partition") return Glyph::Swimlane;
    return Glyph::Unknown;
}
bool connectorKind(QString kind) {
    kind = kind.toLower();
    if (kind.endsWith("view")) kind.chop(4);
    return QStringList{"association", "inherit", "inheritance", "generalization", "uses", "dependency", "realize", "realization", "message", "transition", "link", "attach", "objectflow", "controlflow"}.contains(kind);
}
class SymbolItem final : public QGraphicsItem {
public:
    SymbolItem(PresentationRecord record, Glyph shape, QString title, QStringList attributes, QStringList operations)
        : record_(std::move(record)), shape_(shape), title_(std::move(title)), attributes_(std::move(attributes)), operations_(std::move(operations)) {
        setFlags(ItemIsSelectable | ItemSendsGeometryChanges);
        if (!record_.readOnly && shape_ != Glyph::Unknown) setFlag(ItemIsMovable);
        setPos(record_.geometry.x, record_.geometry.y);
        setData(0, record_.id.value);
        setToolTip(record_.kind + "\n" + record_.id.value);
    }
    QRectF boundingRect() const override { return body().adjusted(-4,-4,4,4); }
    QRectF body() const { return {-record_.geometry.width/2, -record_.geometry.height/2, record_.geometry.width, record_.geometry.height}; }
    QPointF nativeAttachment(QPointF point) const { return mapToScene(point-QPointF(record_.geometry.x,record_.geometry.y)); }
    void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override {
        p->save();
        p->scale(nativeUnitsPerPixel, nativeUnitsPerPixel);
        auto font = p->font(); font.setPointSizeF(10); p->setFont(font);
        const auto palette = QApplication::palette();
        QPen pen(palette.color(isSelected() ? QPalette::Highlight : QPalette::Text));
        pen.setWidthF(isSelected() ? 2 : 1);
        p->setPen(pen); p->setBrush(palette.brush(QPalette::Base));
        auto r = body(); r = {r.x()/nativeUnitsPerPixel,r.y()/nativeUnitsPerPixel,r.width()/nativeUnitsPerPixel,r.height()/nativeUnitsPerPixel};
        const double cx = r.center().x(), cy = r.center().y();
        bool titleDrawn = false;
        switch (shape_) {
        case Glyph::Actor: {
            const double head = std::min(r.width()/5, r.height()/8);
            p->drawEllipse(QPointF(cx,r.top()+head+2),head,head);
            p->drawLine(QPointF(cx,r.top()+2*head+2),QPointF(cx,cy+head));
            p->drawLine(QPointF(cx-r.width()/3,cy-head),QPointF(cx+r.width()/3,cy-head));
            p->drawLine(QPointF(cx,cy+head),QPointF(cx-r.width()/3,r.bottom()-20));
            p->drawLine(QPointF(cx,cy+head),QPointF(cx+r.width()/3,r.bottom()-20));
            p->drawText(QRectF(r.left(),r.bottom()-20,r.width(),20),Qt::AlignCenter,title_); titleDrawn=true; break;
        }
        case Glyph::UseCase: p->drawEllipse(r); break;
        case Glyph::Class: {
            p->drawRect(r); double header = std::min(30.0,r.height()/3);
            double attrHeight = std::max(20.0,(r.height()-header)/2);
            p->drawLine(QPointF(r.left(),r.top()+header),QPointF(r.right(),r.top()+header));
            p->drawLine(QPointF(r.left(),r.top()+header+attrHeight),QPointF(r.right(),r.top()+header+attrHeight));
            p->drawText(QRectF(r.left()+4,r.top(),r.width()-8,header),Qt::AlignCenter,title_);
            p->drawText(QRectF(r.left()+4,r.top()+header+3,r.width()-8,attrHeight-4),Qt::AlignLeft|Qt::AlignTop,attributes_.join('\n'));
            p->drawText(QRectF(r.left()+4,r.top()+header+attrHeight+3,r.width()-8,r.height()-header-attrHeight-4),Qt::AlignLeft|Qt::AlignTop,operations_.join('\n')); titleDrawn=true; break;
        }
        case Glyph::Object: {
            p->drawRect(r); auto font = p->font(); font.setUnderline(true); p->setFont(font); break;
        }
        case Glyph::Lifeline: {
            QRectF head(r.left(),r.top(),r.width(),std::min(40.0,r.height()/3)); p->drawRect(head);
            p->drawText(head,Qt::AlignCenter,title_); auto dashed=pen; dashed.setStyle(Qt::DashLine);p->setPen(dashed);
            p->drawLine(QPointF(cx,head.bottom()),QPointF(cx,r.bottom()));
            const auto activation = record_.properties.value("activation");
            if (activation == "TRUE" || activation == "true") { p->setPen(pen); p->drawRect(QRectF(cx-5,head.bottom()+10,10,std::max(10.0,r.height()/2))); }
            titleDrawn=true;break;
        }
        case Glyph::State: case Glyph::Activity: p->drawRoundedRect(r,shape_==Glyph::Activity ? r.height()/2 : 12,shape_==Glyph::Activity ? r.height()/2 : 12);break;
        case Glyph::Initial: p->setBrush(palette.brush(QPalette::Text));p->drawEllipse(r);titleDrawn=true;break;
        case Glyph::Final: p->drawEllipse(r);p->setBrush(palette.brush(QPalette::Text));p->drawEllipse(r.adjusted(5,5,-5,-5));titleDrawn=true;break;
        case Glyph::Decision: p->drawPolygon(QPolygonF(QVector<QPointF>{QPointF(cx,r.top()),QPointF(r.right(),cy),QPointF(cx,r.bottom()),QPointF(r.left(),cy)}));break;
        case Glyph::Fork: p->setBrush(palette.brush(QPalette::Text));p->drawRect(r);titleDrawn=true;break;
        case Glyph::Component: p->drawRect(r.adjusted(10,0,0,0));p->drawRect(QRectF(r.left(),r.top()+r.height()/5,20,12));p->drawRect(QRectF(r.left(),r.top()+r.height()*3/5,20,12));break;
        case Glyph::Node: {
            auto front=r.adjusted(0,12,-12,0);p->drawRect(front);
            p->drawPolygon(QPolygonF(QVector<QPointF>{front.topLeft(),QPointF(front.left()+12,r.top()),QPointF(r.right(),r.top()),front.topRight()}));
            p->drawPolygon(QPolygonF(QVector<QPointF>{front.topRight(),QPointF(r.right(),r.top()),QPointF(r.right(),r.bottom()-12),front.bottomRight()}));break;
        }
        case Glyph::Package: p->drawRect(r.adjusted(0,15,0,0));p->drawRect(QRectF(r.left(),r.top(),r.width()/2,15));break;
        case Glyph::Note: {
            p->drawPolygon(QPolygonF(QVector<QPointF>{r.topLeft(),QPointF(r.right()-12,r.top()),QPointF(r.right(),r.top()+12),r.bottomRight(),r.bottomLeft()}));
            p->drawPolyline(QPolygonF(QVector<QPointF>{QPointF(r.right()-12,r.top()),QPointF(r.right()-12,r.top()+12),QPointF(r.right(),r.top()+12)}));break;
        }
        case Glyph::Swimlane: p->drawRect(r);p->drawLine(QPointF(r.left(),r.top()+25),QPointF(r.right(),r.top()+25));p->drawText(QRectF(r.left(),r.top(),r.width(),25),Qt::AlignCenter,title_);titleDrawn=true;break;
        case Glyph::Unknown: pen.setStyle(Qt::DashLine);p->setPen(pen);p->drawRect(r);p->drawLine(r.topLeft(),r.bottomRight());p->drawText(r.adjusted(4,4,-4,-4),Qt::AlignCenter|Qt::TextWordWrap,QStringLiteral("Unsupported: ")+record_.kind+"\n"+title_);titleDrawn=true;break;
        }
        if (!titleDrawn) p->drawText(r.adjusted(5,5,-5,-5),Qt::AlignCenter|Qt::TextWordWrap,title_);
        p->restore();
    }
private:
    PresentationRecord record_; Glyph shape_; QString title_; QStringList attributes_, operations_;
};
class ConnectorItem final : public QGraphicsPathItem {
public:
    PresentationRecord record;
    QString title, relationKind;
    ConnectorItem(PresentationRecord value, QString label, QString kind) : record(std::move(value)), title(std::move(label)), relationKind(std::move(kind)) {
        setFlag(ItemIsSelectable);setData(0,record.id.value);setZValue(-1);
    }
    void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override {
        auto font=p->font();font.setPointSizeF(10*nativeUnitsPerPixel);p->setFont(font);
        auto palette=QApplication::palette();QPen pen(palette.color(isSelected()?QPalette::Highlight:QPalette::Text));pen.setWidthF((isSelected()?2:1)*nativeUnitsPerPixel);
        auto kind=relationKind.toLower();
        if (kind.contains("dependency") || kind.contains("uses") || kind.contains("realiz") || kind.contains("attach")) pen.setStyle(Qt::DashLine);
        p->setPen(pen);p->setBrush(Qt::NoBrush);p->drawPath(path());
        if (path().elementCount()<2) return;
        QPointF end=path().pointAtPercent(1), near=path().pointAtPercent(0.98);
        QLineF line(near,end);double angle=std::atan2(line.dy(),line.dx());
        QPointF a=end-QPointF(std::cos(angle-0.5)*13*nativeUnitsPerPixel,std::sin(angle-0.5)*13*nativeUnitsPerPixel);
        QPointF b=end-QPointF(std::cos(angle+0.5)*13*nativeUnitsPerPixel,std::sin(angle+0.5)*13*nativeUnitsPerPixel);
        pen.setStyle(Qt::SolidLine);p->setPen(pen);
        if (kind.contains("inherit") || kind.contains("general") || kind.contains("realiz")) {p->setBrush(palette.brush(QPalette::Base));p->drawPolygon(QPolygonF(QVector<QPointF>{end,a,b}));}
        else if (!kind.contains("association") && !kind.contains("link") && !kind.contains("attach")) p->drawPolyline(QPolygonF(QVector<QPointF>{a,end,b}));
        if (record.properties.value("aggregate") == "TRUE" || record.properties.value("aggregation") == "composite") {
            QPointF start=path().pointAtPercent(0), next=path().pointAtPercent(0.02);double t=std::atan2(next.y()-start.y(),next.x()-start.x());
            auto at=[&](double x,double y){return start+QPointF(x*std::cos(t)-y*std::sin(t),x*std::sin(t)+y*std::cos(t));};
            p->setBrush(record.properties.value("aggregation")=="composite"?palette.brush(QPalette::Text):palette.brush(QPalette::Base));p->drawPolygon(QPolygonF(QVector<QPointF>{start,at(9,5),at(18,0),at(9,-5)}));
        }
        QPointF mid=path().pointAtPercent(0.5);p->drawText(QRectF(mid.x()+4*nativeUnitsPerPixel,mid.y()-24*nativeUnitsPerPixel,300*nativeUnitsPerPixel,22*nativeUnitsPerPixel),Qt::AlignLeft,title);
    }
};
QPointF border(QGraphicsItem *item,QPointF toward) {
    auto *symbol=dynamic_cast<SymbolItem*>(item);
    auto rect=symbol?symbol->mapRectToScene(symbol->body()):item->sceneBoundingRect();auto center=rect.center();auto d=toward-center;
    if (qFuzzyIsNull(d.x()) && qFuzzyIsNull(d.y())) return center;
    double sx=qFuzzyIsNull(d.x())?1e30:rect.width()/2/std::abs(d.x());double sy=qFuzzyIsNull(d.y())?1e30:rect.height()/2/std::abs(d.y());
    return center+d*std::min(sx,sy);
}
}
DiagramScene::DiagramScene(QObject *parent) : QGraphicsScene(parent) {}
void DiagramScene::setProjection(const Projection &projection) {
    clear();items_.clear();gesture_.clear();projection_=projection;
    QHash<ElementId,ElementRecord> elements;QHash<RelationId,RelationRecord> relations;
    for (const auto &e:projection.elements) elements.insert(e.id,e);
    for (const auto &r:projection.relations) relations.insert(r.id,r);
    for (const auto &record:projection.presentations) {
        auto geometry=record.geometry;
        if (!std::isfinite(geometry.x)||!std::isfinite(geometry.y)||!std::isfinite(geometry.width)||!std::isfinite(geometry.height)||geometry.width<=0||geometry.height<=0) {
            auto *warning=addText(tr("Invalid source geometry: %1").arg(record.id.value));warning->setFlag(QGraphicsItem::ItemIsSelectable);warning->setData(0,record.id.value);items_.insert(record.id,warning);
            emit diagnostic({Severity::Warning,"geometry",tr("Presentation cannot be rendered at invalid native geometry"),{},record.id.value,{}});continue;
        }
        auto relation=relations.value(record.relation);auto element=elements.value(record.element);
        if (connectorKind(record.kind) || !record.relation.isEmpty()) {
            if (!connectorKind(record.kind) && !connectorKind(relation.kind)) emit diagnostic({Severity::Warning,"glyph",tr("Unknown connector notation: %1").arg(record.kind),{},record.id.value,{}});
            auto *item=new ConnectorItem(record,record.label.isEmpty()?relation.name:record.label,relation.kind.isEmpty()?record.kind:relation.kind);addItem(item);items_.insert(record.id,item);continue;
        }
        auto shape=glyph(record.kind);if(shape==Glyph::Unknown) shape=glyph(element.kind);
        QStringList attributes,operations;
        for(const auto &child:projection.elements) if(child.owner==element.id) {
            auto text=child.name;auto type=child.properties.value("type");if(!type.isEmpty())text+=": "+type;
            if(child.kind=="ClassAttribute" || child.kind=="Attribute")attributes.append(text);
            if(child.kind=="Operation")operations.append(text+"()");
        }
        auto *item=new SymbolItem(record,shape,record.label.isEmpty()?element.name:record.label,attributes,operations);addItem(item);items_.insert(record.id,item);
        if(shape==Glyph::Unknown)emit diagnostic({Severity::Warning,"glyph",tr("Unknown presentation notation retained: %1").arg(record.kind),{},record.id.value,{}});
    }
    updateConnectors();setSceneRect(itemsBoundingRect().adjusted(-60,-60,60,60));
}
QGraphicsItem *DiagramScene::presentationItem(const PresentationId &id) const {return items_.value(id);}
QVector<PresentationId> DiagramScene::selectedPresentations() const {QVector<PresentationId> ids;for(auto *item:selectedItems())if(!item->data(0).toString().isEmpty())ids.append({item->data(0).toString()});return ids;}
void DiagramScene::beginGesture(){gesture_.clear();for(auto it=items_.cbegin();it!=items_.cend();++it)if(it.value()->flags().testFlag(QGraphicsItem::ItemIsMovable))gesture_.insert(it.key(),it.value()->pos());}
QVector<Command> DiagramScene::endGesture(){QVector<Command> commands;for(const auto &record:projection_.presentations){auto *item=items_.value(record.id);if(item&&gesture_.contains(record.id)&&item->pos()!=gesture_.value(record.id)){auto geometry=record.geometry;geometry.x=item->pos().x();geometry.y=item->pos().y();commands.append({SetGeometry{record.id,geometry}});}}gesture_.clear();return commands;}
void DiagramScene::cancelGesture(){for(auto it=gesture_.cbegin();it!=gesture_.cend();++it)if(auto *item=items_.value(it.key()))item->setPos(it.value());gesture_.clear();updateConnectors();}
void DiagramScene::updateConnectors(){
    QHash<ElementId,QGraphicsItem*> subjects;
    bool subjectsReady=false;
    for(auto *item:items_)if(auto *edge=dynamic_cast<ConnectorItem*>(item)){
        auto *from=items_.value(edge->record.client),*to=items_.value(edge->record.supplier);
        if(!from||!to){
            if(!subjectsReady){
                subjects.reserve(projection_.presentations.size());
                for(const auto &record:projection_.presentations)if(!record.element.isEmpty())subjects.insert(record.element,items_.value(record.id));
                subjectsReady=true;
            }
            for(const auto &relation:projection_.relations)if(relation.id==edge->record.relation&&relation.endpoints.size()>=2){from=subjects.value(relation.endpoints[0]);to=subjects.value(relation.endpoints[1]);break;}
        }
        QVector<WorldPoint> fallback;
        const auto *points=&edge->record.route;
        if(points->size()<2){
            if(from&&to){if(from==to){auto r=from->sceneBoundingRect();fallback={{r.right(),r.center().y()},{r.right()+40,r.center().y()},{r.right()+40,r.top()-30},{r.center().x(),r.top()-30},{r.center().x(),r.top()}};}else{const auto a=from->sceneBoundingRect().center(),b=to->sceneBoundingRect().center();fallback={{a.x(),a.y()},{b.x(),b.y()}};}}
            if(fallback.isEmpty()){const auto &g=edge->record.geometry;fallback={{g.x-g.width/2,g.y},{g.x+g.width/2,g.y}};}
            points=&fallback;
        }
        auto first=QPointF(points->first().x,points->first().y),last=QPointF(points->last().x,points->last().y);
        if(points==&fallback){
            if(from)first=border(from,{points->at(1).x,points->at(1).y});
            if(to)last=border(to,{points->at(points->size()-2).x,points->at(points->size()-2).y});
        }else{
            // Native vertices/attachments are authoritative, even when off-box.
            // Preview only the translation that SetGeometry will persist; do not
            // silently replace stored endpoints with a different center-ray clip.
            if(auto *symbol=dynamic_cast<SymbolItem*>(from))first=symbol->nativeAttachment(first);
            if(auto *symbol=dynamic_cast<SymbolItem*>(to))last=symbol->nativeAttachment(last);
        }
        QPainterPath path;path.moveTo(first);
        for(qsizetype i=1;i+1<points->size();++i)path.lineTo(points->at(i).x,points->at(i).y);
        path.lineTo(last);edge->setPath(path);
    }
}
DiagramView::DiagramView(DiagramScene *scene,QWidget *parent):QGraphicsView(scene,parent){setRenderHint(QPainter::Antialiasing);setDragMode(RubberBandDrag);setTransformationAnchor(AnchorUnderMouse);setResizeAnchor(AnchorViewCenter);setFocusPolicy(Qt::StrongFocus);}
DiagramScene *DiagramView::diagramScene()const{return static_cast<DiagramScene*>(scene());}
void DiagramView::zoomIn(){if(transform().m11()<8)scale(1.2,1.2);}
void DiagramView::zoomOut(){if(transform().m11()>0.08)scale(1/1.2,1/1.2);}
void DiagramView::fitDiagram(){fitInView(scene()->itemsBoundingRect().adjusted(-20,-20,20,20),Qt::KeepAspectRatio);}
void DiagramView::mousePressEvent(QMouseEvent *event){if(event->button()==Qt::MiddleButton){panning_=true;lastPan_=event->pos();setCursor(Qt::ClosedHandCursor);event->accept();return;}if(event->button()==Qt::LeftButton)diagramScene()->beginGesture();QGraphicsView::mousePressEvent(event);}
void DiagramView::mouseMoveEvent(QMouseEvent *event){if(panning_){auto delta=event->pos()-lastPan_;lastPan_=event->pos();horizontalScrollBar()->setValue(horizontalScrollBar()->value()-delta.x());verticalScrollBar()->setValue(verticalScrollBar()->value()-delta.y());event->accept();return;}QGraphicsView::mouseMoveEvent(event);diagramScene()->updateConnectors();}
void DiagramView::mouseReleaseEvent(QMouseEvent *event){if(event->button()==Qt::MiddleButton){panning_=false;unsetCursor();event->accept();return;}QGraphicsView::mouseReleaseEvent(event);if(event->button()==Qt::LeftButton){auto commands=diagramScene()->endGesture();if(!commands.isEmpty())emit commandsRequested(commands);}}
void DiagramView::mouseDoubleClickEvent(QMouseEvent *event){auto *edge=dynamic_cast<ConnectorItem*>(itemAt(event->pos()));if(edge&&!edge->record.readOnly){auto points=edge->record.route;auto point=mapToScene(event->pos());if(points.size()<2){auto path=edge->path();auto start=path.pointAtPercent(0),end=path.pointAtPercent(1);points={{start.x(),start.y()},{end.x(),end.y()}};}qsizetype best=1;double distance=1e300;for(qsizetype i=1;i<points.size();++i){QPointF a(points[i-1].x,points[i-1].y),b(points[i].x,points[i].y);auto d=b-a;double len=QPointF::dotProduct(d,d);double t=len==0?0:std::clamp(QPointF::dotProduct(point-a,d)/len,0.0,1.0);auto diff=point-(a+d*t);double candidate=QPointF::dotProduct(diff,diff);if(candidate<distance){distance=candidate;best=i;}}points.insert(best,{point.x(),point.y()});emit commandsRequested({Command{SetRoute{edge->record.id,points}}});event->accept();return;}QGraphicsView::mouseDoubleClickEvent(event);}
void DiagramView::wheelEvent(QWheelEvent *event){if(event->modifiers().testFlag(Qt::ControlModifier)){if(event->angleDelta().y()>0)zoomIn();else zoomOut();event->accept();return;}QGraphicsView::wheelEvent(event);}
void DiagramView::keyPressEvent(QKeyEvent *event){if(event->key()==Qt::Key_Escape){diagramScene()->cancelGesture();event->accept();return;}if(event->matches(QKeySequence::Delete)){emit removeRequested();event->accept();return;}if(event->matches(QKeySequence::SelectAll)){for(auto *item:scene()->items())if(item->flags().testFlag(QGraphicsItem::ItemIsSelectable))item->setSelected(true);event->accept();return;}QGraphicsView::keyPressEvent(event);}
}
