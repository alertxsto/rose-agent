#include "desktop/DiagramScene.h"
#include <QSignalSpy>
#include <QtTest>
#include <QGraphicsPathItem>
using namespace rose;
using namespace rose::desktop;
class CanvasTest : public QObject {
    Q_OBJECT
private slots:
    void nativeRouteEndpointsRemainAuthoritativeDuringMoves() {
        Projection p;
        PresentationRecord customer; customer.id = {"customer"}; customer.kind = "ClassView";
        customer.geometry = {100, 1100, 600, 440};
        PresentationRecord person; person.id = {"person"}; person.kind = "ClassView";
        person.geometry = {800, 400, 600, 400};
        PresentationRecord inheritance; inheritance.id = {"inheritance"}; inheritance.kind = "InheritView";
        inheritance.geometry = {0, 0, 1, 1}; inheritance.client = customer.id; inheritance.supplier = person.id;
        // This is the user's actual native route. The off-box terminal must not
        // look connected in Qt while a native export retains a different point.
        inheritance.route = {{400, 1100}, {700, 1000}, {1000, 800}};
        p.presentations = {customer, person, inheritance};
        DiagramScene scene; scene.setProjection(p);
        auto *edge = dynamic_cast<QGraphicsPathItem *>(scene.presentationItem(inheritance.id));
        QVERIFY(edge);
        auto points = [&]() {
            QVector<QPointF> result;
            for (int i = 0; i < edge->path().elementCount(); ++i) {
                const auto point = edge->path().elementAt(i); result.append({point.x, point.y});
            }
            return result;
        };
        QCOMPARE(points(), (QVector<QPointF>{{400, 1100}, {700, 1000}, {1000, 800}}));
        scene.beginGesture();
        scene.presentationItem(customer.id)->setPos(80, 1140);
        scene.updateConnectors();
        QCOMPARE(points(), (QVector<QPointF>{{380, 1140}, {700, 1000}, {1000, 800}}));
        scene.cancelGesture();
        QCOMPARE(points(), (QVector<QPointF>{{400, 1100}, {700, 1000}, {1000, 800}}));
        QVERIFY(scene.endGesture().isEmpty());
    }
    void centerCoordinatesAndIndependentAppearances() {
        Projection p; p.revision = 7;
        p.elements = {{ElementId{"class"}, "Class", "Order"}};
        PresentationRecord first; first.id = {"first"}; first.element = {"class"}; first.kind = "ClassView";
        first.geometry = {300, 200, 140, 80};
        auto second = first; second.id = {"second"}; second.geometry.x = 600;
        p.presentations = {first, second};
        DiagramScene scene; scene.setProjection(p);
        QCOMPARE(scene.presentationItem(first.id)->pos(), QPointF(300, 200));
        scene.beginGesture();
        scene.presentationItem(first.id)->setPos(320, 240);
        auto commands = scene.endGesture();
        QCOMPARE(commands.size(), 1);
        auto move = std::get<SetGeometry>(commands[0].payload);
        QCOMPARE(move.id, first.id);
        QCOMPARE(move.geometry.x, 320.0);
        QCOMPARE(move.geometry.y, 240.0);
        QCOMPARE(move.geometry.width, 140.0);
        QCOMPARE(scene.presentationItem(second.id)->pos(), QPointF(600, 200));
        QCOMPARE(p.presentations[0].geometry.x, 300.0);
        QVERIFY(scene.endGesture().isEmpty());
    }
    void cancelledGestureRestoresSource() {
        Projection p;
        PresentationRecord record; record.id = {"one"}; record.kind = "ClassView"; record.geometry = {10,20,120,80};
        p.presentations = {record};
        DiagramScene scene; scene.setProjection(p);
        scene.beginGesture(); scene.presentationItem(record.id)->setPos(50,70);
        scene.cancelGesture();
        QCOMPARE(scene.presentationItem(record.id)->pos(), QPointF(10,20));
        QVERIFY(scene.endGesture().isEmpty());
    }
};
QTEST_MAIN(CanvasTest)
#include "tst_canvas.moc"
