#pragma once

#include "Model.h"

namespace rose {

using ObjectId = std::variant<ElementId, RelationId, DiagramId, PresentationId>;
struct CreateElement {
    QString clientId, kind, name;
    ElementId owner;
    UnitId unit;
};
struct RenameElement { ElementId id; QString name; };
struct SetProperty { ObjectId id; QString key; EncodedValue value; };
struct SetOwner { ElementId id, owner; };
struct CopyElements {
    QVector<ElementId> ids;
    ElementId owner;
    QMap<ElementId, QString> clientIds;
};
struct CreateRelation {
    QString clientId, kind, name;
    ElementId owner;
    QVector<ElementId> endpoints;
    QMap<QString, EncodedValue> properties;
};
struct ReconnectRelation { RelationId id; QVector<ElementId> endpoints; };
struct CreateDiagram {
    QString clientId, kind, name;
    ElementId owner;
    UnitId unit;
};
struct AddPresentation {
    QString clientId;
    DiagramId diagram;
    ObjectId subject = ElementId{};
    Geometry geometry;
};
struct SetGeometry { PresentationId id; Geometry geometry; };
struct SetRoute { PresentationId id; QVector<WorldPoint> points; };
struct SetMessageOrder { ElementId id; qint64 ordinal = 0; };
struct RemovePresentation { PresentationId id; };
struct DeleteElement { ElementId id; QStringList acknowledgedDependents; };
using CommandPayload = std::variant<CreateElement, RenameElement, SetProperty, SetOwner,
    CopyElements, CreateRelation, ReconnectRelation, CreateDiagram, AddPresentation,
    SetGeometry, SetRoute, SetMessageOrder, RemovePresentation, DeleteElement>;
struct Command { CommandPayload payload; };

} // namespace rose
