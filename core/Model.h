#pragma once

#include "PetalDocument.h"
#include <memory>

namespace rose {

struct ElementRecord {
    ElementId id;
    QString kind, name;
    ElementId owner;
    UnitId unit;
    QMap<QString, QString> properties;
    QMap<QString, QString> propertyTypes;
    bool readOnly = false;
    friend bool operator==(const ElementRecord &, const ElementRecord &) = default;
};
struct RelationRecord {
    RelationId id;
    QString kind, name;
    ElementId owner;
    QVector<ElementId> endpoints;
    UnitId unit;
    QMap<QString, QString> properties;
    QMap<QString, QString> propertyTypes;
    bool readOnly = false;
    friend bool operator==(const RelationRecord &, const RelationRecord &) = default;
};
struct PresentationRecord {
    PresentationId id;
    DiagramId diagram;
    ElementId element;
    RelationId relation;
    PresentationId parent, client, supplier;
    QString kind, label;
    quint64 localLabel = 0;
    Geometry geometry;
    QVector<WorldPoint> route;
    UnitId unit;
    QMap<QString, QString> properties;
    QMap<QString, QString> propertyTypes;
    bool readOnly = false;
    friend bool operator==(const PresentationRecord &, const PresentationRecord &) = default;
};
struct DiagramRecord {
    DiagramId id;
    QString kind, name;
    ElementId owner;
    UnitId unit;
    QVector<PresentationId> presentations;
    bool readOnly = false;
    friend bool operator==(const DiagramRecord &, const DiagramRecord &) = default;
};
struct UnitSummary {
    UnitId id;
    QString path;
    bool writable = false, resolved = true;
};
struct Projection {
    Revision revision = 0;
    bool dirty = false, canUndo = false, canRedo = false;
    QString path;
    QVector<ElementRecord> elements;
    QVector<RelationRecord> relations;
    QVector<DiagramRecord> diagrams;
    QVector<PresentationRecord> presentations;
    QVector<Diagnostic> diagnostics;
    QVector<UnitSummary> units;
    qsizetype total = 0;
};
struct Query {
    enum class Kind { ModelTree, ElementsById, Search, DiagramById, Neighborhood, Diagnostics };
    Kind kind = Kind::ModelTree;
    QVector<ElementId> elements;
    QString search;
    DiagramId diagram;
    ElementId focus;
    int depth = 1;
    qsizetype offset = 0, limit = 250;
};
struct UnitDocument {
    UnitId id;
    QString path;
    PetalDocument document;
    bool writable = true;
};
struct DiffEntry {
    QString kind, objectId, field, before, after;
};
template<class Record> struct RecordChange {
    std::optional<Record> before, after;
};
struct SourceCopy {
    QString sourceId;
    QMap<QString, QString> ids;
};
struct ModelDelta {
    QVector<RecordChange<ElementRecord>> elements;
    QVector<RecordChange<RelationRecord>> relations;
    QVector<RecordChange<DiagramRecord>> diagrams;
    QVector<RecordChange<PresentationRecord>> presentations;
    QMap<QString, QString> newIds;
    QMap<QString, SourceCopy> sourceCopies;
    QMap<DiagramId, ElementId> newMechanisms;
    QVector<DiffEntry> diff;
    QVector<Diagnostic> diagnostics;
    bool isEmpty() const noexcept {
        return elements.isEmpty() && relations.isEmpty() && diagrams.isEmpty() && presentations.isEmpty();
    }
};

struct Command;
class Model {
public:
    static Outcome<std::unique_ptr<Model>> fromDocuments(QVector<UnitDocument> documents);
    ~Model();
    Model(Model &&) noexcept;
    Model &operator=(Model &&) noexcept;
    Model(const Model &) = delete;
    Model &operator=(const Model &) = delete;
    Outcome<Projection> inspect(const Query &, Revision revision) const;
    Outcome<ModelDelta> stage(const QVector<Command> &) const;
    Status applyDelta(const ModelDelta &, bool forward = true);
    Outcome<QMap<UnitId, QByteArray>> serialize() const;
    const QVector<UnitDocument> &documents() const;
private:
    struct Impl;
    explicit Model(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

} // namespace rose
