#pragma once
#include <QString>
class QGraphicsScene;
class QWidget;
namespace rose::desktop {
void printDiagram(QGraphicsScene *, QWidget *, bool preview = false);
bool exportDiagram(QGraphicsScene *, const QString &path, QString *error);
}
