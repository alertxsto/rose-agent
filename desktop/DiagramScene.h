#pragma once
#include "core/Command.h"
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHash>
namespace rose::desktop {
class DiagramScene final : public QGraphicsScene {
    Q_OBJECT
public:
    explicit DiagramScene(QObject *parent = nullptr);
    void setProjection(const Projection &);
    QGraphicsItem *presentationItem(const PresentationId &) const;
    QVector<PresentationId> selectedPresentations() const;
    const Projection &projection() const { return projection_; }
    void beginGesture();
    QVector<Command> endGesture();
    void cancelGesture();
    void updateConnectors();
signals:
    void diagnostic(rose::Diagnostic diagnostic);
private:
    Projection projection_;
    QHash<PresentationId, QGraphicsItem *> items_;
    QHash<PresentationId, QPointF> gesture_;
};
class DiagramView final : public QGraphicsView {
    Q_OBJECT
public:
    explicit DiagramView(DiagramScene *, QWidget *parent = nullptr);
    DiagramScene *diagramScene() const;
    void zoomIn();
    void zoomOut();
    void fitDiagram();
signals:
    void commandsRequested(QVector<rose::Command> commands);
    void removeRequested();
protected:
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
private:
    bool panning_ = false;
    QPoint lastPan_;
};
}
