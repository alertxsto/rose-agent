#include "Model.h"
#include "Command.h"
#include <QDateTime>
#include <QSet>
#include <QRegularExpression>
#include "SourceEncoding.h"
#include <QUuid>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <type_traits>
#include <utility>

namespace rose {
namespace {
[[noreturn]] void fail(ErrorCode code, const QString &message, const QString &file = {}) {
    throw WorkspaceError{code, message, file};
}
QString token(const PetalDocument &d, const PetalPart &p) {
    auto value = d.text(p);
    if (auto e = std::get_if<WorkspaceError>(&value)) throw *e;
    return std::get<QString>(value);
}
QString property(const PetalDocument &d, int n, const QString &key) {
    for (const auto &p : d.nodes()[n].properties)
        if (d.raw(p.key) == key.toLatin1()) return token(d, p.value);
    return {};
}
QString kind(const PetalDocument &d, int n) { return d.kind(n).toLatin1String(); }
bool elementKind(const QString &k) {
    return k == "Design" || k == "Class_Category" || k == "Class" || k == "ClassAttribute"
        || k == "Operation" || k == "Parameter" || k == "SubSystem" || k == "Processes";
}
bool relationKind(const QString &k) { return k == "Association" || k == "Inheritance_Relationship"; }
bool diagramKind(const QString &k) {
    return k == "ClassDiagram" || k == "UseCaseDiagram" || k == "Module_Diagram" || k == "Process_Diagram";
}
bool viewKind(const QString &k) { return k == "ClassView" || k == "CategoryView" || k == "InheritView" || k == "AssociationViewNew"; }
QString typeOf(const PetalDocument &d, const PetalPart &p) {
    if (p.child >= 0) return "opaque";
    if (p.kind == AtomKind::Quoted || p.kind == AtomKind::Text) return "string";
    if (p.kind == AtomKind::Reference) return "reference";
    const auto s = token(d, p);
    if (s == "TRUE" || s == "FALSE") return "boolean";
    bool ok = false; s.toLongLong(&ok); if (ok) return "integer";
    s.toDouble(&ok); return ok ? "number" : "word";
}
template<class R> void readProperties(const PetalDocument &d, int n, R &r) {
    for (const auto &p : d.nodes()[n].properties) {
        const QString key = d.raw(p.key).toLatin1String();
        if (r.propertyTypes.contains(key)) fail(ErrorCode::UnsafeRewrite, "Duplicate property on supported object: " + key);
        const auto type = typeOf(d, p.value); r.propertyTypes.insert(key, type);
        // Structured values stay in the immutable syntax tree, not copied into every ancestor record.
        if (type != "opaque") r.properties.insert(key, token(d, p.value));
    }
}
QByteArray quote(const QString &s, const QByteArray &encoding) {
    if (s.contains(QChar::Null)) fail(ErrorCode::InvalidCommand, "Text contains NUL");
    QByteArray bytes;
    if (encoding == "ASCII") {
        for (auto c : s) if (c.unicode() > 127) fail(ErrorCode::UnsupportedEncoding, "Text is not representable in ASCII");
        bytes = s.toLatin1();
    } else {
        auto result = sourceEncoding::encode(s, encoding);
        if (const auto *error = std::get_if<sourceEncoding::Error>(&result))
            fail(ErrorCode::UnsupportedEncoding, *error == sourceEncoding::Error::Unsupported
                ? "Unsupported source encoding" : "Text is not representable in source encoding");
        bytes = std::get<QByteArray>(std::move(result));
    }
    QByteArray result("\"");
    for (char c : bytes) { if (c == '\\' || c == '"') result += '\\'; result += c; }
    result += '"'; return result;
}
QString valueType(const EncodedValue &v) {
    if (std::holds_alternative<QString>(v)) return "string";
    if (std::holds_alternative<bool>(v)) return "boolean";
    if (std::holds_alternative<qint64>(v)) return "integer";
    if (std::holds_alternative<double>(v)) return "number";
    return "reference";
}
QString valueText(const EncodedValue &v) {
    return std::visit([](const auto &x) -> QString {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, QString>) return x;
        else if constexpr (std::is_same_v<T, bool>) return x ? "TRUE" : "FALSE";
        else if constexpr (std::is_same_v<T, LocalReference>) return '@' + QString::number(x.value);
        else if constexpr (std::is_same_v<T, double>) {
            if (!std::isfinite(x)) fail(ErrorCode::InvalidCommand, "Numeric property must be finite");
            return QString::number(x, 'g', std::numeric_limits<double>::max_digits10);
        } else return QString::number(x);
    }, v);
}
QByteArray encode(const QString &s, const QString &type, const QByteArray &encoding) {
    if (type == "string") return quote(s, encoding);
    if (type == "opaque") fail(ErrorCode::UnsafeRewrite, "Structured property requires a native profile rule");
    if (type == "boolean" && s != "TRUE" && s != "FALSE") fail(ErrorCode::InvalidCommand, "Invalid boolean");
    bool ok = true;
    if (type == "integer") s.toLongLong(&ok);
    if (type == "number") { const double n = s.toDouble(&ok); ok = ok && std::isfinite(n); }
    if (type == "reference") { if (!s.startsWith('@')) ok = false; else s.mid(1).toULongLong(&ok); }
    if (type == "word") ok = !s.isEmpty() && !s.contains(QRegularExpression("[\\s()\"\\\\,]"));
    if (!ok) fail(ErrorCode::InvalidCommand, "Invalid typed native value");
    return s.toLatin1();
}
QByteArray number(double n) {
    if (!std::isfinite(n)) fail(ErrorCode::InvalidCommand, "Geometry must be finite");
    return QByteArray::number(n, 'g', std::numeric_limits<double>::max_digits10);
}
QByteArray point(const WorldPoint &p) { return '(' + number(p.x) + ", " + number(p.y) + ')'; }
WorldPoint reattach(const WorldPoint &point, const Geometry &before, const Geometry &after) {
    return {after.x + (point.x - before.x) * after.width / before.width,
        after.y + (point.y - before.y) * after.height / before.height};
}
WorldPoint boundaryAttachment(const Geometry &geometry, const WorldPoint &toward) {
    const double dx = toward.x - geometry.x, dy = toward.y - geometry.y;
    if (dx == 0 && dy == 0) return {geometry.x + geometry.width / 2, geometry.y};
    const double sx = dx == 0 ? std::numeric_limits<double>::infinity() : geometry.width / (2 * std::abs(dx));
    const double sy = dy == 0 ? std::numeric_limits<double>::infinity() : geometry.height / (2 * std::abs(dy));
    const double scale = std::min(sx, sy);
    return {geometry.x + dx * scale, geometry.y + dy * scale};
}
WorldPoint readPoint(const PetalDocument &d, const PetalPart &p) {
    if (p.child < 0) fail(ErrorCode::InvalidSyntax, "Expected native point tuple");
    const auto &parts = d.nodes()[p.child].parts;
    if (parts.size() != 3 || parts[1].kind != AtomKind::Comma) fail(ErrorCode::InvalidSyntax, "Invalid native point tuple");
    bool a = false, b = false; const auto x = token(d, parts[0]).toDouble(&a), y = token(d, parts[2]).toDouble(&b);
    if (!a || !b || !std::isfinite(x) || !std::isfinite(y)) fail(ErrorCode::InvalidSyntax, "Invalid point coordinates");
    return {x, y};
}
// Observed in bundled TODOLIST (Petal44) and Eclipse QVT (Petal50).
// The association's two RoleViews run from a shared junction to each class.
QVector<int> associationRoles(const PetalDocument &d, int n) {
    QVector<int> roles;
    for (const auto &p : d.nodes()[n].properties) if (d.raw(p.key) == "roleview_list" && p.value.child >= 0)
        for (const auto &part : d.nodes()[p.value.child].parts)
            if (part.child >= 0 && kind(d, part.child) == "RoleView") roles.append(part.child);
    if (roles.size() != 2) fail(ErrorCode::UnsupportedProfile, "Only binary AssociationViewNew RoleViews are supported");
    return roles;
}
QVector<WorldPoint> nativeRoute(const PetalDocument &d, int n) {
    QVector<WorldPoint> route;
    for (const auto &p : d.nodes()[n].properties) if (d.raw(p.key) == "vertices" && p.value.child >= 0)
        for (const auto &part : d.nodes()[p.value.child].parts) if (part.child >= 0) route.append(readPoint(d, part));
    if (route.isEmpty()) for (const auto &key : {QByteArray("origin_attachment"), QByteArray("terminal_attachment")})
        for (const auto &p : d.nodes()[n].properties) if (d.raw(p.key) == key) route.append(readPoint(d, p.value));
    return route;
}
QVector<WorldPoint> roleRoute(const QVector<WorldPoint> &route, int end) {
    if (route.size() < 2) fail(ErrorCode::InvalidCommand, "Association route requires at least two points");
    const qsizetype split = route.size() / 2;
    // Two-point routes get a genuine central junction, not a zero-length role.
    if (route.size() == 2) {
        const WorldPoint center{(route[0].x + route[1].x) / 2, (route[0].y + route[1].y) / 2};
        return {center, route[end]};
    }
    QVector<WorldPoint> result;
    if (end == 0) for (qsizetype i = split; i >= 0; --i) result.append(route[i]);
    else for (qsizetype i = split; i < route.size(); ++i) result.append(route[i]);
    return result;
}
QString freshId() {
    return QString::number(QDateTime::currentSecsSinceEpoch(), 16).rightJustified(8, '0').toUpper()
        + QUuid::createUuid().toString(QUuid::Id128).left(4).toUpper();
}
QString presentationId(const UnitId &unit, const DiagramId &diagram, quint64 label) {
    return QString::fromLatin1(QUrl::toPercentEncoding(unit.value)) + '#'
        + QString::fromLatin1(QUrl::toPercentEncoding(diagram.value)) + '@' + QString::number(label);
}
struct Location { int unit = -1, node = -1; bool stub = false; };
template<class R> struct Records {
    using Key = decltype(R{}.id);
    QMap<Key, R> live, original;
    QMap<Key, Location> source;
};
template<class R> struct Overlay {
    using Key = decltype(R{}.id);
    const QMap<Key, R> &base;
    QMap<Key, std::optional<R>> edits;
    const R *get(const Key &key) const {
        auto changed = edits.constFind(key);
        if (changed != edits.cend()) return changed->has_value() ? &changed->value() : nullptr;
        auto found = base.constFind(key); return found == base.cend() ? nullptr : &found.value();
    }
    R require(const Key &key) const {
        const auto *r = get(key); if (!r) fail(ErrorCode::InvalidCommand, "Unknown object: " + key.value);
        return *r;
    }
    void put(R r) {
        const auto id = r.id;
        edits.insert(id, std::move(r));
    }
    void remove(const Key &key) { edits.insert(key, std::nullopt); }
    template<class F> void each(F fn) const {
        // Snapshot only touched keys: self-updates during traversal must not be visited again.
        const auto initialEdits = edits.keys();
        for (auto i = base.cbegin(); i != base.cend(); ++i) {
            if (std::binary_search(initialEdits.cbegin(), initialEdits.cend(), i.key())) continue;
            const auto changed = edits.constFind(i.key());
            if (changed == edits.cend()) fn(i.value());
            else if (*changed) fn(changed->value());
        }
        for (const auto &key : initialEdits) if (const auto *r = get(key)) fn(*r);
    }
};
struct State {
    Overlay<ElementRecord> elements;
    Overlay<RelationRecord> relations;
    Overlay<DiagramRecord> diagrams;
    Overlay<PresentationRecord> presentations;
};
QString qualified(const State &s, ElementId id) {
    QStringList names; QSet<ElementId> seen;
    while (!id.isEmpty()) {
        if (seen.contains(id)) fail(ErrorCode::InvalidCommand, "Ownership cycle");
        seen.insert(id);
        const auto *r = s.elements.get(id); if (!r) fail(ErrorCode::DanglingReference, "Missing qualified-name owner");
        if (r->kind != "Design" && !r->name.isEmpty()) names.prepend(r->name);
        id = r->owner;
    }
    return names.join("::");
}
QString listKey(const QString &child, const QString &owner) {
    if (child == "Class" && owner == "Class") return "nestedClasses";
    if ((child == "Class" || child == "Class_Category" || child == "Association") && owner == "Class_Category") return "logical_models";
    if (child == "ClassAttribute" && owner == "Class") return "class_attributes";
    if (child == "Operation" && owner == "Class") return "operations";
    if (child == "Parameter" && owner == "Operation") return "parameters";
    if (child == "Inheritance_Relationship" && owner == "Class") return "superclasses";
    if (child == "ClassDiagram" && owner == "Class_Category") return "logical_presentations";
    if (viewKind(child) && owner == "ClassDiagram") return "items";
    return {};
}
QString listType(const QString &key) {
    if (key == "class_attributes") return "class_attribute_list";
    if (key == "operations") return "Operations";
    if (key == "parameters") return "Parameters";
    if (key == "nestedClasses") return "nestedClasses";
    if (key == "superclasses") return "inheritance_relationship_list";
    if (key == "items") return "diagram_item_list";
    return "unit_reference_list";
}
void writable(bool readOnly, const QString &id) { if (readOnly) fail(ErrorCode::AccessDenied, "Object is read-only: " + id); }
void validGeometry(const Geometry &g) {
    number(g.x); number(g.y); number(g.width); number(g.height);
    if (g.width <= 0 || g.height <= 0) fail(ErrorCode::InvalidCommand, "Presentation dimensions must be positive");
}
template<class R> QVector<RecordChange<R>> changes(const Overlay<R> &o) {
    QVector<RecordChange<R>> result;
    for (auto i = o.edits.cbegin(); i != o.edits.cend(); ++i) {
        std::optional<R> before; auto b = o.base.constFind(i.key()); if (b != o.base.cend()) before = b.value();
        if (before != i.value()) result.append({before, i.value()});
    }
    return result;
}
template<class R> void loadChanges(Overlay<R> &o, const QVector<RecordChange<R>> &changes, bool forward) {
    QSet<typename Overlay<R>::Key> seen;
    for (const auto &c : changes) {
        const auto &before = forward ? c.before : c.after, &after = forward ? c.after : c.before;
        if (!before && !after) fail(ErrorCode::InvalidCommand, "Empty record change");
        const auto id = before ? before->id : after->id;
        if (seen.contains(id) || (after && after->id != id)) fail(ErrorCode::InvalidCommand, "Duplicate or changed delta identity");
        seen.insert(id); const auto *live = o.get(id);
        if ((before && (!live || *live != *before)) || (!before && live)) fail(ErrorCode::StaleRevision, "Delta no longer matches object: " + id.value);
        if (before) writable(before->readOnly, id.value);
        if (before && after && (before->kind != after->kind || before->unit != after->unit || before->readOnly != after->readOnly))
            fail(ErrorCode::UnsafeRewrite, "Delta cannot change native kind, source unit or access policy");
        if constexpr (std::is_same_v<R, RelationRecord>) {
            if (before && after && before->kind == "Association")
                for (int role = 0; role < 2; ++role) {
                    const auto key = QString("roles.%1.quid").arg(role);
                    if (before->properties.value(key) != after->properties.value(key))
                        fail(ErrorCode::UnsafeRewrite, "Association Role identities are read-only");
                }
        }
        if (after) o.put(*after); else o.remove(id);
    }
}
template<class R> void commit(Records<R> &r, const Overlay<R> &o) {
    for (auto i = o.edits.cbegin(); i != o.edits.cend(); ++i) { if (i.value()) r.live.insert(i.key(), *i.value()); else r.live.remove(i.key()); }
}
}

struct Model::Impl {
    QVector<UnitDocument> documents;
    Records<ElementRecord> elements;
    Records<RelationRecord> relations;
    Records<DiagramRecord> diagrams;
    Records<PresentationRecord> presentations;
    QMap<UnitId, int> units;
    QVector<QMap<int, QString>> nodeIds;
    QVector<QByteArray> lineEndings;
    QMap<QString, SourceCopy> copies;
    QVector<Diagnostic> diagnostics;
    State state() const { return {{elements.live, {}}, {relations.live, {}}, {diagrams.live, {}}, {presentations.live, {}}}; }
    State originalState() const { return {{elements.original, {}}, {relations.original, {}}, {diagrams.original, {}}, {presentations.original, {}}}; }
    QSet<QString> nativeIds;
    const UnitDocument &unit(UnitId id) const {
        auto found = units.constFind(id); if (found == units.cend()) fail(ErrorCode::MissingUnit, "Unknown unit: " + id.value);
        return documents[*found];
    }
    [[noreturn]] void sourceError(ErrorCode code, const QString &message, UnitId unitId, const QString &objectId, const QString &key = {}) const {
        const auto &u = unit(unitId);
        qsizetype offset = 0;
        const auto index = units.value(unitId);
        for (auto i = nodeIds[index].cbegin(); i != nodeIds[index].cend(); ++i) if (i.value() == objectId) {
            offset = u.document.nodes()[i.key()].span.offset;
            if (!key.isEmpty()) {
                int target = i.key(); QString field = key;
                if (key.startsWith("roles.")) {
                    const int requestedRole = key.section('.', 1, 1).toInt();
                    field = key.section('.', 2, 2);
                    int roleIndex = 0;
                    for (const auto &p : u.document.nodes()[target].properties) if (u.document.raw(p.key) == "roles" && p.value.child >= 0)
                        for (const auto &role : u.document.nodes()[p.value.child].parts)
                            if (role.child >= 0 && kind(u.document, role.child) == "Role" && roleIndex++ == requestedRole) target = role.child;
                }
                const auto value = u.document.property(target, field.toLatin1());
                if (value) offset = value->span().offset;
            }
            break;
        }
        auto e = u.document.error(code, message, offset); e.file = u.path; throw e;
    }
    template<class R> void add(Records<R> &records, R r, Location loc) {
        auto found = records.live.constFind(r.id);
        if (found != records.live.cend()) {
            const auto old = records.source.value(r.id);
            if (!old.stub && !loc.stub) sourceError(ErrorCode::DanglingReference, "Conflicting semantic identity: " + r.id.value, documents[loc.unit].id, r.id.value, "quid");
            if (found->kind != r.kind) sourceError(ErrorCode::DanglingReference, "Unit stub kind conflicts with loaded definition: " + r.id.value, documents[loc.unit].id, r.id.value, "quid");
            if (old.stub && !loc.stub) {
                if constexpr (!std::is_same_v<R, PresentationRecord>) r.owner = found->owner;
                records.live.insert(r.id, r); records.original.insert(r.id, r); records.source.insert(r.id, loc);
            }
            else if constexpr (!std::is_same_v<R, PresentationRecord>) {
                if (loc.stub && r.kind != "Design" && !r.owner.isEmpty()) {
                    if (!found->owner.isEmpty() && found->owner != r.owner) fail(ErrorCode::UnsafeRewrite, "Unit has conflicting native ownership stubs: " + r.id.value);
                    if (found->owner.isEmpty()) {
                        auto canonical = found.value(); canonical.owner = r.owner;
                        records.live.insert(r.id, canonical); records.original.insert(r.id, canonical);
                    }
                }
            }
            return;
        }
        records.live.insert(r.id, r); records.original.insert(r.id, r); records.source.insert(r.id, loc);
    }
    void validate(const State &s) const;
    void preflight(const State &s) const;
    void opaqueSafety(const QSet<QString> &ids, bool copying, const Location *subtree = nullptr) const;
    Outcome<QMap<UnitId, QByteArray>> serialize(const State &s, const QMap<QString, SourceCopy> &sourceCopies) const;
};

Model::Model(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Model::~Model() = default;
Model::Model(Model &&) noexcept = default;
Model &Model::operator=(Model &&) noexcept = default;
const QVector<UnitDocument> &Model::documents() const { return impl_->documents; }

Outcome<std::unique_ptr<Model>> Model::fromDocuments(QVector<UnitDocument> documents) {
    try {
        if (documents.isEmpty()) fail(ErrorCode::MissingUnit, "No native documents supplied");
        auto impl = std::make_unique<Impl>(); impl->documents = std::move(documents);
        const auto rootHeaders = impl->documents.front().document.objectsOfKind("Petal");
        if (rootHeaders.size() != 1)
            fail(ErrorCode::UnsupportedProfile, "The root document requires exactly one explicit supported Petal44 or Petal50 header", impl->documents.front().path);
        const QString version = property(impl->documents.front().document, rootHeaders.front(), "version");
        if (version != "44" && version != "50")
            fail(ErrorCode::UnsupportedProfile, "Model projection supports the Petal44 and Petal50 class profiles", impl->documents.front().path);
        QSet<QString> classIds, supportedSubjects, opaqueSubjects, opaqueRelations;
        for (const auto &unit : impl->documents) for (int n = 0; n < unit.document.nodes().size(); ++n) {
            const auto k = kind(unit.document, n), id = property(unit.document, n, "quid");
            if (k == "Class") classIds.insert(id);
            if (elementKind(k)) supportedSubjects.insert(id);
            else if (!id.isEmpty()) opaqueSubjects.insert(id);
        }
        impl->nodeIds.resize(impl->documents.size());
        for (int u = 0; u < impl->documents.size(); ++u) {
            const auto &unit = impl->documents[u]; const auto &d = unit.document;
            const auto lf = d.bytes().indexOf('\n');
            impl->lineEndings.append(lf >= 0
                ? (lf > 0 && d.bytes()[lf - 1] == '\r' ? QByteArrayLiteral("\r\n") : QByteArrayLiteral("\n"))
                : (d.bytes().contains('\r') ? QByteArrayLiteral("\r") : QByteArrayLiteral("\n")));
            if (impl->units.contains(unit.id)) fail(ErrorCode::InvalidCommand, "Duplicate unit identity", unit.path);
            impl->units.insert(unit.id, u);
            const auto headers = d.objectsOfKind("Petal");
            if (headers.size() > 1) fail(ErrorCode::UnsupportedProfile, "A controlled unit has multiple Petal headers", unit.path);
            for (int p : headers) if (property(d, p, "version") != version)
                fail(ErrorCode::UnsupportedProfile, "Mixed Petal versions in a controlled-unit set are unsafe to edit", unit.path);
            impl->diagnostics.append({Severity::Information, "NativeClassProfile", "Detected Petal" + version + " source-preserving class profile; other families remain opaque", unit.path, {}, {}});
            QVector<ElementId> owner(d.nodes().size()); QVector<DiagramId> diagram(d.nodes().size());
            for (int n = 0; n < d.nodes().size(); ++n) {
                const auto &node = d.nodes()[n]; const auto k = kind(d, n);
                if (node.parent >= 0) { owner[n] = owner[node.parent]; diagram[n] = diagram[node.parent]; }
                const auto id = property(d, n, "quid");
                if (!id.isEmpty()) impl->nativeIds.insert(id);
                if (elementKind(k) || relationKind(k) || diagramKind(k)) {
                    if (id.isEmpty()) fail(ErrorCode::DanglingReference, "Supported object has no quid: " + k, unit.path);
                    impl->nodeIds[u].insert(n, id);
                    const Location loc{u, n, property(d, n, "is_loaded") == "FALSE" && !property(d, n, "file_name").isEmpty()};
                    const QString name = d.name(n) ? d.name(n)->text() : QString{};
                    if (elementKind(k)) {
                        ElementRecord r{ElementId{id}, k, name, owner[n], unit.id, {}, {}, !unit.writable || loc.stub};
                        readProperties(d, n, r); impl->add(impl->elements, r, loc); owner[n] = r.id;
                    } else if (relationKind(k)) {
                        RelationRecord r{RelationId{id}, k, name, owner[n], {}, unit.id, {}, {}, !unit.writable || loc.stub};
                        readProperties(d, n, r);
                        if (k == "Inheritance_Relationship") r.endpoints = {owner[n], ElementId{property(d, n, "quidu")}};
                        else for (const auto &p : node.properties) if (d.raw(p.key) == "roles" && p.value.child >= 0) {
                            for (const auto &role : d.nodes()[p.value.child].parts) if (role.child >= 0 && kind(d, role.child) == "Role") {
                                const auto key = QString("roles.%1.quid").arg(r.endpoints.size());
                                if (r.propertyTypes.contains(key))
                                    impl->sourceError(ErrorCode::UnsafeRewrite, "Native property collides with read-only Role identity metadata", unit.id, r.id.value, key);
                                r.properties.insert(key, property(d, role.child, "quid")); r.propertyTypes.insert(key, "string");
                                r.endpoints.append(ElementId{property(d, role.child, "quidu")});
                            }
                        }
                        if (r.endpoints.size() != 2) {
                            impl->nodeIds[u].remove(n); opaqueRelations.insert(id);
                            impl->diagnostics.append({Severity::Warning, "OpaqueRelationship", "Non-binary relationship is preserved without editing", unit.path, id, node.span});
                            continue;
                        }
                        bool classRelation = true;
                        for (const auto &endpoint : r.endpoints) {
                            if (!classIds.contains(endpoint.value) && !opaqueSubjects.contains(endpoint.value))
                                fail(ErrorCode::DanglingReference, "Native relationship endpoint is missing: " + endpoint.value, unit.path);
                            classRelation = classRelation && classIds.contains(endpoint.value);
                        }
                        if (!classRelation) {
                            impl->nodeIds[u].remove(n); opaqueRelations.insert(id);
                            impl->diagnostics.append({Severity::Warning, "OpaqueRelationship", "Relationship with an unsupported non-class endpoint is preserved without editing", unit.path, id, node.span});
                            continue;
                        }
                        impl->add(impl->relations, r, loc);
                    } else {
                        DiagramRecord r{DiagramId{id}, k, name, owner[n], unit.id, {}, !unit.writable || loc.stub};
                        impl->add(impl->diagrams, r, loc); diagram[n] = r.id;
                    }
                } else if (viewKind(k)) {
                    if (diagram[n].isEmpty()) fail(ErrorCode::DanglingReference, "Presentation is outside a diagram", unit.path);
                    quint64 label = 0; bool hasLabel = false;
                    for (qsizetype p = 2; p < node.headerParts; ++p) if (node.parts[p].kind == AtomKind::Reference) { label = token(d, node.parts[p]).mid(1).toULongLong(); hasLabel = true; }
                    if (!hasLabel) fail(ErrorCode::DanglingReference, "Presentation has no local label", unit.path);
                    PresentationRecord r; r.id = PresentationId{presentationId(unit.id, diagram[n], label)};
                    if (impl->presentations.live.contains(r.id)) fail(ErrorCode::DanglingReference, "Duplicate diagram-local presentation label", unit.path);
                    r.diagram = diagram[n]; r.kind = k; r.localLabel = label; r.unit = unit.id; r.readOnly = !unit.writable;
                    const auto subject = property(d, n, "quidu");
                    if (k == "InheritView" || k == "AssociationViewNew") r.relation = RelationId{subject}; else r.element = ElementId{subject};
                    if (!r.element.isEmpty() && !supportedSubjects.contains(subject)) {
                        impl->diagnostics.append({Severity::Warning, "OpaquePresentation", "Presentation of an unsupported subject is preserved without editing", unit.path, subject, node.span});
                        continue;
                    }
                    readProperties(d, n, r);
                    for (const auto &p : node.properties) {
                        const auto key = d.raw(p.key);
                        if (key == "location") { const auto xy = readPoint(d, p.value); r.geometry.x = xy.x; r.geometry.y = xy.y; }
                        if (key == "width" || key == "height") {
                            bool ok = false; const auto value = token(d, p.value).toDouble(&ok); if (!ok || !std::isfinite(value)) fail(ErrorCode::InvalidSyntax, "Invalid presentation dimensions", unit.path);
                            if (key == "width") r.geometry.width = value; else r.geometry.height = value;
                        }
                        if (key == "vertices" && p.value.child >= 0) for (const auto &v : d.nodes()[p.value.child].parts) if (v.child >= 0) r.route.append(readPoint(d, v));
                        if (key == "label" && p.value.child >= 0) r.label = property(d, p.value.child, "label");
                    }
                    if (k == "InheritView") r.route = nativeRoute(d, n);
                    if (r.label.isEmpty() && node.headerParts > 2 && k != "ClassView") r.label = token(d, node.parts[2]);
                    impl->nodeIds[u].insert(n, r.id.value); impl->add(impl->presentations, r, {u, n, false});
                }
            }
        }
        for (auto &p : impl->presentations.live) {
            if (!p.element.isEmpty() && p.label.isEmpty()) { const auto e = impl->elements.live.constFind(p.element); if (e != impl->elements.live.cend()) p.label = e->name; }
            impl->presentations.original.insert(p.id, p);
            auto diagram = impl->diagrams.live.find(p.diagram); if (diagram != impl->diagrams.live.end()) diagram->presentations.append(p.id);
        }
        for (auto &d : impl->diagrams.live) impl->diagrams.original.insert(d.id, d);
        QVector<PresentationId> opaqueViews;
        for (const auto &p : impl->presentations.live) if (!p.relation.isEmpty() && !impl->relations.live.contains(p.relation)) {
            if (!opaqueRelations.contains(p.relation.value)) fail(ErrorCode::DanglingReference, "Presentation references a missing native relationship: " + p.relation.value);
            opaqueViews.append(p.id);
        }
        for (const auto &id : opaqueViews) {
            const auto p = impl->presentations.live.value(id);
            const auto loc = impl->presentations.source.value(id);
            impl->diagnostics.append({Severity::Warning, "OpaquePresentation", "Presentation of an unsupported relationship is preserved without editing", impl->documents[loc.unit].path, p.relation.value, impl->documents[loc.unit].document.nodes()[loc.node].span});
            impl->nodeIds[loc.unit].remove(loc.node);
            impl->diagrams.live[p.diagram].presentations.removeAll(id);
            impl->diagrams.original[p.diagram].presentations.removeAll(id);
            impl->presentations.live.remove(id); impl->presentations.original.remove(id); impl->presentations.source.remove(id);
        }
        for (auto &p : impl->presentations.live) {
            const auto loc = impl->presentations.source.value(p.id); const auto &d = impl->documents[loc.unit].document;
            if (p.kind == "AssociationViewNew") {
                const auto roles = associationRoles(d, loc.node);
                const auto relation = impl->relations.live.value(p.relation);
                QVector<WorldPoint> routes[2];
                for (int end = 0; end < 2; ++end) {
                    int role = -1;
                    const auto roleId = relation.properties.value(QString("roles.%1.quid").arg(end));
                    for (int candidate : roles) if (property(d, candidate, "quidu") == roleId) {
                        if (role >= 0) fail(ErrorCode::UnsafeRewrite, "Duplicate association RoleView identity");
                        role = candidate;
                    }
                    if (role < 0) fail(ErrorCode::DanglingReference, "Association RoleView does not reference a semantic Role");
                    const auto ref = property(d, role, "supplier");
                    if (!ref.startsWith('@')) fail(ErrorCode::DanglingReference, "Association RoleView has no native class endpoint");
                    const PresentationId target{presentationId(p.unit, p.diagram, ref.mid(1).toULongLong())};
                    if (end == 0) p.client = target; else p.supplier = target;
                    routes[end] = nativeRoute(d, role);
                }
                if (!routes[0].isEmpty() && !routes[1].isEmpty()) {
                    if (routes[0].front() != routes[1].front()) fail(ErrorCode::UnsupportedProfile, "Association RoleViews do not share a supported junction");
                    for (qsizetype i = routes[0].size(); i > 0; --i) p.route.append(routes[0][i - 1]);
                    for (qsizetype i = 1; i < routes[1].size(); ++i) p.route.append(routes[1][i]);
                }
            }
            for (auto key : {QString("client"), QString("supplier"), QString("Parent_View")}) {
                const auto ref = property(d, loc.node, key); if (!ref.startsWith('@')) continue;
                const PresentationId target{presentationId(p.unit, p.diagram, ref.mid(1).toULongLong())};
                if (!impl->presentations.live.contains(target)) fail(ErrorCode::DanglingReference, "Missing supported local presentation endpoint: " + target.value);
                if (key == "client") p.client = target; else if (key == "supplier") p.supplier = target; else p.parent = target;
            }
            impl->presentations.original.insert(p.id, p);
        }
        if (impl->elements.live.isEmpty()) fail(ErrorCode::UnsupportedProfile, "The root profile contains no supported semantic model");
        impl->validate(impl->state());
        return std::unique_ptr<Model>(new Model(std::move(impl)));
    } catch (const WorkspaceError &e) { return e; }
}

void Model::Impl::validate(const State &s) const {
    QSet<QString> identities;
    auto identity = [&](const QString &id) { if (id.isEmpty() || identities.contains(id)) fail(ErrorCode::DanglingReference, "Duplicate or empty semantic identity: " + id); identities.insert(id); };
    s.elements.each([&](const ElementRecord &r) {
        identity(r.id.value); unit(r.unit);
        if (!r.owner.isEmpty() && !s.elements.get(r.owner)) fail(ErrorCode::DanglingReference, "Missing owner: " + r.owner.value);
        const auto quidu = r.properties.value("quidu");
        if (!quidu.isEmpty() && !s.elements.get(ElementId{quidu}))
            sourceError(ErrorCode::DanglingReference, "Missing typed element reference: " + quidu, r.unit, r.id.value, "quidu");
    });
    QHash<ElementId, int> ownership;
    s.elements.each([&](const ElementRecord &r) {
        QVector<ElementId> path; auto id = r.id;
        while (!id.isEmpty() && ownership.value(id) != 2) {
            if (ownership.value(id) == 1) fail(ErrorCode::InvalidCommand, "Ownership cycle");
            ownership.insert(id, 1); path.append(id); id = s.elements.get(id)->owner;
        }
        for (const auto &visited : path) ownership.insert(visited, 2);
    });
    s.relations.each([&](const RelationRecord &r) {
        identity(r.id.value); unit(r.unit);
        if (!s.elements.get(r.owner)) fail(ErrorCode::DanglingReference, "Missing relationship owner");
        if (r.endpoints.size() != 2) fail(ErrorCode::InvalidCommand, "Class relationship requires two endpoints");
        if (r.kind == "Association") for (int role = 0; role < 2; ++role)
            identity(r.properties.value(QString("roles.%1.quid").arg(role)));
        for (int endpoint = 0; endpoint < r.endpoints.size(); ++endpoint) {
            const auto id = r.endpoints[endpoint];
            if (!s.elements.get(id) || s.elements.get(id)->kind != "Class")
                sourceError(ErrorCode::DanglingReference, "Class relationship endpoint is absent or not a class: " + id.value,
                    r.unit, r.id.value, r.kind == "Association" ? QString("roles.%1.quidu").arg(endpoint) : QString("quidu"));
        }
        if (r.kind == "Inheritance_Relationship" && r.owner != r.endpoints.front()) fail(ErrorCode::InvalidCommand, "Inheritance client must be its native class owner");
    });
    s.diagrams.each([&](const DiagramRecord &r) {
        identity(r.id.value); unit(r.unit); if (!s.elements.get(r.owner)) fail(ErrorCode::DanglingReference, "Missing diagram owner");
        QSet<PresentationId> seen;
        for (const auto &id : r.presentations) { const auto *p = s.presentations.get(id); if (seen.contains(id) || !p || p->diagram != r.id) fail(ErrorCode::DanglingReference, "Invalid diagram presentation membership"); seen.insert(id); }
    });
    s.presentations.each([&](const PresentationRecord &r) {
        const auto *diagram = s.diagrams.get(r.diagram); if (!diagram || !diagram->presentations.contains(r.id) || diagram->unit != r.unit) fail(ErrorCode::DanglingReference, "Presentation diagram is missing or in another unit");
        if ((!r.element.isEmpty()) == (!r.relation.isEmpty())) fail(ErrorCode::DanglingReference, "Presentation requires exactly one supported subject");
        if (!r.element.isEmpty() && !s.elements.get(r.element)) fail(ErrorCode::DanglingReference, "Presentation subject is missing");
        if (!r.relation.isEmpty() && !s.relations.get(r.relation)) fail(ErrorCode::DanglingReference, "Presentation relationship is missing");
        for (const auto &id : {r.parent, r.client, r.supplier}) if (!id.isEmpty()) { const auto *p = s.presentations.get(id); if (!p || p->diagram != r.diagram) fail(ErrorCode::DanglingReference, "Presentation endpoint is missing or outside diagram"); }
        if (r.kind == "AssociationViewNew" || r.kind == "InheritView") {
            const auto relation = s.relations.require(r.relation);
            if ((r.kind == "AssociationViewNew") != (relation.kind == "Association"))
                fail(ErrorCode::DanglingReference, "Native relationship presentation kind does not match its subject");
            for (int end = 0; end < 2; ++end) {
                const auto target = s.presentations.get(end == 0 ? r.client : r.supplier);
                if (!target || target->element != relation.endpoints[end])
                    fail(ErrorCode::DanglingReference, "Native relationship view endpoint does not match its semantic Role");
            }
        }
        validGeometry(r.geometry); for (const auto &p : r.route) { number(p.x); number(p.y); }
    });
}

void Model::Impl::preflight(const State &s) const {
    auto properties = [&](const auto &overlay) {
        for (const auto &r : overlay.edits) if (r) {
            const auto &d = unit(r->unit); writable(r->readOnly, r->id.value);
            for (auto i = r->properties.cbegin(); i != r->properties.cend(); ++i) if (r->propertyTypes.value(i.key()) != "opaque") encode(i.value(), r->propertyTypes.value(i.key()), d.document.encoding());
        }
    };
    properties(s.elements); properties(s.relations); properties(s.presentations);
    for (const auto &r : s.elements.edits) if (r) {
        if (r->name.contains('\n') || r->name.contains('\r')) fail(ErrorCode::InvalidCommand, "Names cannot contain line breaks");
        const auto loc = elements.source.constFind(r->id);
        if (loc != elements.source.cend() && !r->name.isEmpty() && !documents[loc->unit].document.name(loc->node))
            fail(ErrorCode::UnsafeRewrite, "Unnamed native object has no supported positional-name mutation rule");
        quote(r->name, unit(r->unit).document.encoding());
        for (int u = 0; u < documents.size(); ++u) for (auto i = nodeIds[u].cbegin(); i != nodeIds[u].cend(); ++i) {
            const auto baseline = elements.original.constFind(r->id);
            if (baseline == elements.original.cend() || baseline->name == r->name) continue;
            if (i.value() != r->id.value || (loc != elements.source.cend() && loc->unit == u && loc->node == i.key())) continue;
            const auto name = documents[u].document.name(i.key());
            if (name && name->text() != r->name) {
                writable(!documents[u].writable, r->id.value);
                quote(r->name, documents[u].document.encoding());
            }
        }
    }
    for (const auto &r : s.diagrams.edits) if (r) {
        writable(r->readOnly, r->id.value);
        if (r->name.contains('\n') || r->name.contains('\r')) fail(ErrorCode::InvalidCommand, "Diagram names cannot contain line breaks");
        quote(r->name, unit(r->unit).document.encoding());
    }
    for (const auto &r : s.relations.edits) if (r) {
        if (r->name.contains('\n') || r->name.contains('\r')) fail(ErrorCode::InvalidCommand, "Relationship names cannot contain line breaks");
        quote(r->name, unit(r->unit).document.encoding());
    }
    for (const auto &r : s.presentations.edits) if (r) {
        quote(r->label, unit(r->unit).document.encoding());
        if (!r->element.isEmpty()) quote(qualified(s, r->element), unit(r->unit).document.encoding());
    }
    auto checkContainment = [&](const auto &overlay, const auto &source, const auto &original) {
        for (auto i = overlay.edits.cbegin(); i != overlay.edits.cend(); ++i) if (!i.value()) {
            const auto loc = source.constFind(i.key());
            if (loc == source.cend()) continue;
            const auto &d = documents[loc->unit].document;
            const int parent = d.nodes()[loc->node].parent;
            if (parent < 0 || d.nodes()[parent].parts.isEmpty() || d.raw(d.nodes()[parent].parts[0].span) != "list")
                fail(ErrorCode::UnsafeRewrite, "Native root/property-owned object cannot be deep-deleted");
        }
        for (const auto &r : overlay.edits) if (r) {
            using R = std::decay_t<decltype(*r)>;
            QString ownerId;
            if constexpr (std::is_same_v<R, PresentationRecord>) ownerId = r->diagram.value; else ownerId = r->owner.value;
            if (ownerId.isEmpty()) continue;
            QString ownerKind; Location ownerLocation;
            if constexpr (std::is_same_v<R, PresentationRecord>) {
                const auto *owner = s.diagrams.get(r->diagram); ownerKind = owner->kind; ownerLocation = diagrams.source.value(owner->id, Location{});
            } else {
                const auto *owner = s.elements.get(r->owner); ownerKind = owner->kind; ownerLocation = elements.source.value(owner->id, Location{});
            }
            auto prior = original.constFind(r->id);
            bool changedOwner = prior == original.cend();
            if (prior != original.cend()) {
                if constexpr (std::is_same_v<R, PresentationRecord>) changedOwner = prior->diagram != r->diagram;
                else changedOwner = prior->owner != r->owner;
            }
            if (!changedOwner) continue;
            const auto key = listKey(r->kind, ownerKind);
            if (key.isEmpty()) fail(ErrorCode::UnsupportedProfile, "No native containment rule for changed object");
            if (ownerLocation.node >= 0) {
                const auto &d = documents[ownerLocation.unit].document; int found = 0;
                for (const auto &p : d.nodes()[ownerLocation.node].properties) if (d.raw(p.key) == key.toLatin1()) {
                    ++found;
                    if (found > 1 || p.value.child < 0 || d.nodes()[p.value.child].parts.isEmpty() || d.raw(d.nodes()[p.value.child].parts[0].span) != "list")
                        fail(ErrorCode::UnsafeRewrite, "Native owner list is ambiguous or not a list");
                }
            }
            const auto loc = source.constFind(r->id);
            if (loc != source.cend() && prior != original.cend()) {
                const auto &d = documents[loc->unit].document; int parent = d.nodes()[loc->node].parent;
                if (parent < 0 || d.nodes()[parent].parts.isEmpty() || d.raw(d.nodes()[parent].parts[0].span) != "list")
                    fail(ErrorCode::UnsafeRewrite, "Root/property-owned native object cannot move");
            }
        }
    };
    checkContainment(s.elements, elements.source, elements.original);
    checkContainment(s.relations, relations.source, relations.original);
    checkContainment(s.diagrams, diagrams.source, diagrams.original);
    checkContainment(s.presentations, presentations.source, presentations.original);
    for (const auto &r : s.relations.edits) if (r && r->kind == "Inheritance_Relationship" && !relations.source.contains(r->id) && !r->name.isEmpty())
        fail(ErrorCode::UnsupportedProfile, "Native Inheritance_Relationship has no positional name");
}

void Model::Impl::opaqueSafety(const QSet<QString> &ids, bool copying, const Location *subtree) const {
    for (int u = 0; u < documents.size(); ++u) {
        const auto &d = documents[u].document;
        for (int n = 0; n < d.nodes().size(); ++n) {
            const auto &node = d.nodes()[n];
            if (subtree && (u != subtree->unit || node.span.offset < d.nodes()[subtree->node].span.offset || node.span.offset + node.span.length > d.nodes()[subtree->node].span.offset + d.nodes()[subtree->node].span.length)) continue;
            const auto k = kind(d, n);
            bool mappedRole = false;
            if (k == "Role" || k == "RoleView") {
                for (int ancestor = node.parent; ancestor >= 0; ancestor = d.nodes()[ancestor].parent) {
                    const auto ancestorKind = kind(d, ancestor);
                    if (ancestorKind == "Association" || ancestorKind == "AssociationViewNew") {
                        mappedRole = nodeIds[u].contains(ancestor); break;
                    }
                }
            }
            if (copying && !elementKind(k) && !relationKind(k) && !diagramKind(k) && !property(d, n, "quid").isEmpty()) fail(ErrorCode::UnsafeRewrite, "Copy includes an opaque semantic identity", documents[u].path);
            for (const auto &part : node.parts) {
                if (part.child >= 0 || part.kind == AtomKind::Comma) continue;
                if (copying && part.kind == AtomKind::Reference) fail(ErrorCode::UnsafeRewrite, "Copy includes a local reference without a cloning rule", documents[u].path);
                bool known = false;
                for (const auto &p : node.properties) if (p.value.span.offset == part.span.offset) {
                    const auto key = d.raw(p.key);
                    const bool mapped = nodeIds[u].contains(n);
                    known = key == "quid" && (mapped || mappedRole);
                    known = known || (key == "quidu" && (mapped || mappedRole));
                }
                if (known) continue;
                const auto text = token(d, part);
                for (const auto &id : ids) if (text.contains(id)) fail(ErrorCode::UnsafeRewrite, "Opaque native token may reference affected identity", documents[u].path);
            }
            for (const auto &p : node.properties) {
                const auto key = d.raw(p.key).toLatin1String();
                if (key == "quid" || key == "quidu") continue;
                if (p.value.child >= 0) continue;
                const auto text = token(d, p.value);
                for (const auto &id : ids) if (text.contains(id)) fail(ErrorCode::UnsafeRewrite, "Opaque property may reference affected identity: " + key, documents[u].path);
            }
        }
    }
}

Outcome<Projection> Model::inspect(const Query &q, Revision revision) const {
    try {
        if (q.offset < 0 || q.limit < 0 || q.depth < 0) fail(ErrorCode::InvalidCommand, "Invalid query pagination or depth");
        Projection p; p.revision = revision; p.diagnostics = impl_->diagnostics;
        for (const auto &u : impl_->documents) p.units.append({u.id, u.path, u.writable, true});
        QSet<ElementId> selected; QSet<RelationId> relations; QSet<DiagramId> diagrams; QSet<PresentationId> presentations;
        if (q.kind == Query::Kind::Diagnostics) { p.total = p.diagnostics.size(); p.diagnostics = p.diagnostics.mid(q.offset, q.limit); return p; }
        if (q.kind == Query::Kind::DiagramById) {
            auto d = impl_->diagrams.live.constFind(q.diagram); if (d == impl_->diagrams.live.cend()) fail(ErrorCode::InvalidCommand, "Unknown diagram");
            diagrams.insert(d->id);
            for (auto id : d->presentations) {
                const auto &v = impl_->presentations.live[id]; presentations.insert(id);
                if (!v.element.isEmpty()) selected.insert(v.element);
                if (!v.relation.isEmpty()) { relations.insert(v.relation); for (auto end : impl_->relations.live[v.relation].endpoints) selected.insert(end); }
            }
            for (const auto &r : impl_->relations.live) {
                bool relevant = true;
                for (const auto &end : r.endpoints) relevant = relevant && selected.contains(end);
                if (relevant) relations.insert(r.id);
            }
            // Compartments consume semantic members, including operation parameters,
            // even though only their owning classes have native appearances.
            QSet<ElementId> operations;
            for (const auto &e : impl_->elements.live) {
                if ((e.kind == "ClassAttribute" || e.kind == "Operation") && selected.contains(e.owner)) {
                    selected.insert(e.id);
                    if (e.kind == "Operation") operations.insert(e.id);
                }
            }
            for (const auto &e : impl_->elements.live)
                if (e.kind == "Parameter" && operations.contains(e.owner)) selected.insert(e.id);
        } else if (q.kind == Query::Kind::ElementsById) {
            for (auto id : q.elements) { if (!impl_->elements.live.contains(id)) fail(ErrorCode::InvalidCommand, "Unknown requested element: " + id.value); selected.insert(id); }
        } else if (q.kind == Query::Kind::Neighborhood) {
            if (!impl_->elements.live.contains(q.focus)) fail(ErrorCode::InvalidCommand, "Unknown neighborhood focus");
            selected.insert(q.focus); QSet<ElementId> frontier{q.focus};
            for (int depth = 0; depth < q.depth && !frontier.isEmpty(); ++depth) {
                QSet<ElementId> next;
                for (const auto &e : impl_->elements.live) if (frontier.contains(e.id) || frontier.contains(e.owner)) { if (!e.owner.isEmpty() && !selected.contains(e.owner)) next.insert(e.owner); if (!selected.contains(e.id)) next.insert(e.id); }
                for (const auto &r : impl_->relations.live) { bool hit = frontier.contains(r.owner); for (auto id : r.endpoints) hit = hit || frontier.contains(id); if (hit) { relations.insert(r.id); for (auto id : r.endpoints) if (!selected.contains(id)) next.insert(id); } }
                selected.unite(next); frontier = std::move(next);
            }
            for (const auto &d : impl_->diagrams.live) if (selected.contains(d.owner)) diagrams.insert(d.id);
            for (const auto &v : impl_->presentations.live)
                if (selected.contains(v.element) || relations.contains(v.relation) || diagrams.contains(v.diagram)) presentations.insert(v.id);
            for (auto id : presentations) diagrams.insert(impl_->presentations.live[id].diagram);
        } else {
            for (const auto &e : impl_->elements.live) if (q.kind == Query::Kind::ModelTree || e.name.contains(q.search, Qt::CaseInsensitive) || e.kind.contains(q.search, Qt::CaseInsensitive) || e.id.value == q.search) selected.insert(e.id);
            for (const auto &r : impl_->relations.live) if (q.kind == Query::Kind::ModelTree || r.name.contains(q.search, Qt::CaseInsensitive) || r.id.value == q.search) relations.insert(r.id);
            for (const auto &d : impl_->diagrams.live) if (q.kind == Query::Kind::ModelTree || d.name.contains(q.search, Qt::CaseInsensitive) || d.id.value == q.search) diagrams.insert(d.id);
        }
        p.total = selected.size() + relations.size() + diagrams.size() + presentations.size(); qsizetype row = 0;
        auto appendRow = [&](bool included, const auto &r, auto &out) { if (included) { if (row >= q.offset && row - q.offset < q.limit) out.append(r); ++row; } };
        // Explicit ID selection follows the caller's order, not QMap ordering.
        if (q.kind == Query::Kind::ElementsById) { QSet<ElementId> seen; for (auto id : q.elements) if (!seen.contains(id)) { appendRow(true, impl_->elements.live[id], p.elements); seen.insert(id); } }
        else for (const auto &r : impl_->elements.live) appendRow(selected.contains(r.id), r, p.elements);
        for (const auto &r : impl_->relations.live) appendRow(relations.contains(r.id), r, p.relations);
        for (const auto &r : impl_->diagrams.live) appendRow(diagrams.contains(r.id), r, p.diagrams);
        for (const auto &r : impl_->presentations.live) appendRow(presentations.contains(r.id), r, p.presentations);
        return p;
    } catch (const WorkspaceError &e) { return e; }
}

Outcome<ModelDelta> Model::stage(const QVector<Command> &commands) const {
    try {
        auto s = impl_->state(); ModelDelta delta;
        QSet<QString> allocated;
        QSet<QString> deletedRoleIds;
        QMap<ElementId, ElementRecord> copiedBaselines;
        QSet<PresentationId> explicitRoutes;
        QMap<PresentationId, std::pair<Geometry, Geometry>> authoredRouteGeometry;
        QMap<ElementId, QString> copiedSourcePaths;
        auto allocateId = [&]() {
            QString id;
            do { id = freshId(); } while (impl_->nativeIds.contains(id) || allocated.contains(id));
            allocated.insert(id); return id;
        };
        auto resolve = [&](QString id) { return delta.newIds.value(id, id); };
        auto element = [&](ElementId id) { id.value = resolve(id.value); return id; };
        auto relation = [&](RelationId id) { id.value = resolve(id.value); return id; };
        auto diagram = [&](DiagramId id) { id.value = resolve(id.value); return id; };
        auto presentation = [&](PresentationId id) { id.value = resolve(id.value); return id; };
        auto newId = [&](const QString &client, QString requested = {}) {
            if (client.isEmpty() || delta.newIds.contains(client) || impl_->nativeIds.contains(client) || allocated.contains(client)
                || s.elements.get(ElementId{client}) || s.relations.get(RelationId{client}) || s.diagrams.get(DiagramId{client}) || s.presentations.get(PresentationId{client}))
                fail(ErrorCode::InvalidCommand, "Empty, duplicate or ambiguous client identity: " + client);
            QString id = requested;
            if (id.isEmpty()) id = allocateId();
            delta.newIds.insert(client, id); return id;
        };
        auto ownerFor = [&](ElementId id, const QString &childKind, UnitId requested) {
            auto owner = s.elements.require(element(id)); writable(owner.readOnly, owner.id.value);
            if (listKey(childKind, owner.kind).isEmpty()) fail(ErrorCode::UnsupportedProfile, "Unsupported native containment: " + owner.kind + " / " + childKind);
            if (!requested.isEmpty() && requested != owner.unit) fail(ErrorCode::UnsafeRewrite, "Insertion across controlled-unit ownership is unsupported");
            return owner;
        };
        auto removePresentation = [&](PresentationId id, const QSet<PresentationId> &deleting = {}) {
            auto p = s.presentations.require(id); writable(p.readOnly, id.value);
            auto d = s.diagrams.require(p.diagram); writable(d.readOnly, d.id.value);
            const auto loc = impl_->presentations.source.constFind(id);
            const auto diagramLoc = impl_->diagrams.source.constFind(p.diagram);
            if (loc != impl_->presentations.source.cend() && diagramLoc != impl_->diagrams.source.cend()) {
                const auto &source = impl_->documents[loc->unit].document;
                const auto removed = source.nodes()[loc->node].span, scope = source.nodes()[diagramLoc->node].span;
                for (const auto &node : source.nodes()) {
                    if (node.span.offset < scope.offset || node.span.offset + node.span.length > scope.offset + scope.length
                        || (node.span.offset >= removed.offset && node.span.offset + node.span.length <= removed.offset + removed.length)) continue;
                    bool removedSource = false;
                    for (auto i = impl_->presentations.source.cbegin(); i != impl_->presentations.source.cend(); ++i) {
                        if (i->unit != loc->unit || (!deleting.contains(i.key()) && s.presentations.get(i.key()))) continue;
                        const auto span = source.nodes()[i->node].span;
                        if (node.span.offset >= span.offset && node.span.offset + node.span.length <= span.offset + span.length) { removedSource = true; break; }
                    }
                    if (removedSource) continue;
                    for (const auto &part : node.parts) if (part.kind == AtomKind::Reference && token(source, part).mid(1).toULongLong() == p.localLabel)
                        fail(ErrorCode::UnsafeRewrite, "Another native view/opaque field references this local presentation label");
                }
            }
            d.presentations.removeAll(id); s.diagrams.put(d); s.presentations.remove(id);
        };
        const QSet<QString> structuralKeys{"quid", "quidu", "is_unit", "is_loaded", "file_name", "root_category", "root_usecase_package", "root_subsystem", "process_structure", "logical_models", "logical_presentations", "class_attributes", "operations", "parameters", "nestedClasses", "superclasses", "roles", "items", "location", "width", "height", "vertices", "client", "supplier", "Parent_View"};
        auto setProperty = [&](auto &overlay, const auto &id, const SetProperty &c) {
            auto r = overlay.require(id); writable(r.readOnly, id.value);
            if (c.key.isEmpty() || c.key.contains(QRegularExpression("[^A-Za-z0-9_]")) || structuralKeys.contains(c.key))
                fail(ErrorCode::InvalidCommand, "Property requires its typed structural command: " + c.key);
            const auto type = valueType(c.value), text = valueText(c.value);
            const auto prior = r.propertyTypes.value(c.key);
            if (prior == "opaque" || prior == "reference") fail(ErrorCode::UnsafeRewrite, "Property has structured/reference data without a rewrite rule: " + c.key);
            if (!prior.isEmpty() && prior != type && !(prior == "number" && type == "integer") && !(prior == "word" && type == "string"))
                fail(ErrorCode::InvalidCommand, "Property type mismatch: " + c.key);
            if (type == "reference") fail(ErrorCode::UnsupportedProfile, "Arbitrary local-reference property edits are unsupported");
            if (prior.isEmpty()) {
                static const QSet<QString> strings{"documentation", "stereotype", "type", "initV", "result", "exportControl", "opExportControl", "concurrency", "Constraints"};
                static const QSet<QString> booleans{"abstract", "static", "derived", "global", "ShowCompartmentStereotypes", "IncludeAttribute", "IncludeOperation", "autoResize"};
                if ((!strings.contains(c.key) || type != "string") && (!booleans.contains(c.key) || type != "boolean"))
                    fail(ErrorCode::UnsupportedProfile, "No native scalar insertion rule for property: " + c.key);
            }
            encode(text, type, impl_->unit(r.unit).document.encoding());
            r.properties.insert(c.key, text); r.propertyTypes.insert(c.key, type); overlay.put(r);
        };
        for (const auto &command : commands) std::visit([&](const auto &c) {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, CreateElement>) {
                if (c.kind != "Class" && c.kind != "Class_Category" && c.kind != "ClassAttribute" && c.kind != "Operation" && c.kind != "Parameter")
                    fail(ErrorCode::UnsupportedProfile, "Unsupported native element creation kind: " + c.kind);
                const auto owner = ownerFor(c.owner, c.kind, c.unit);
                ElementRecord r; r.id = ElementId{newId(c.clientId)}; r.kind = c.kind; r.name = c.name; r.owner = owner.id; r.unit = owner.unit;
                r.properties.insert("quid", r.id.value); r.propertyTypes.insert("quid", "string"); s.elements.put(r);
            } else if constexpr (std::is_same_v<T, RenameElement>) {
                auto r = s.elements.require(element(c.id)); writable(r.readOnly, r.id.value); r.name = c.name; s.elements.put(r);
            } else if constexpr (std::is_same_v<T, SetProperty>) {
                std::visit([&](auto id) {
                    using I = decltype(id); id.value = resolve(id.value);
                    if constexpr (std::is_same_v<I, ElementId>) setProperty(s.elements, id, c);
                    else if constexpr (std::is_same_v<I, RelationId>) setProperty(s.relations, id, c);
                    else if constexpr (std::is_same_v<I, PresentationId>) setProperty(s.presentations, id, c);
                    else fail(ErrorCode::UnsupportedProfile, "Diagram property editing has no typed scalar profile");
                }, c.id);
            } else if constexpr (std::is_same_v<T, SetOwner>) {
                auto r = s.elements.require(element(c.id)); writable(r.readOnly, r.id.value);
                const auto owner = ownerFor(c.owner, r.kind, r.unit);
                if (owner.id != r.owner) {
                    impl_->opaqueSafety({r.id.value}, false);
                    const auto loc = impl_->elements.source.constFind(r.id);
                    if (loc != impl_->elements.source.cend()) {
                        const auto &d = impl_->documents[loc->unit].document;
                        const int parent = d.nodes()[loc->node].parent;
                        if (parent < 0 || d.nodes()[parent].parts.isEmpty() || d.raw(d.nodes()[parent].parts.front().span) != "list")
                            fail(ErrorCode::UnsafeRewrite, "Native root/property-owned object cannot move");
                    }
                    r.owner = owner.id; s.elements.put(r);
                }
            } else if constexpr (std::is_same_v<T, CopyElements>) {
                if (c.ids.isEmpty()) fail(ErrorCode::InvalidCommand, "Copy selection is empty");
                QSet<ElementId> selected; QVector<ElementId> roots;
                for (auto id : c.ids) {
                    id = element(id); s.elements.require(id);
                    if (selected.contains(id)) fail(ErrorCode::InvalidCommand, "Duplicate copy selection");
                    selected.insert(id); roots.append(id);
                }
                bool expanded = true;
                while (expanded) { expanded = false; s.elements.each([&](const ElementRecord &r) { if (selected.contains(r.owner) && !selected.contains(r.id)) { selected.insert(r.id); expanded = true; } }); }
                for (auto id : roots) { auto r = s.elements.require(id); if (selected.contains(r.owner)) fail(ErrorCode::InvalidCommand, "Copy roots overlap"); ownerFor(c.owner, r.kind, {}); }
                QSet<QString> affected; for (auto id : selected) affected.insert(id.value);
                QMap<QString, QString> mapping;
                for (auto id : selected) {
                    const auto r = s.elements.require(id);
                    if (!impl_->elements.source.contains(id)) fail(ErrorCode::UnsafeRewrite, "Copy requires an original native source subtree");
                    const auto client = c.clientIds.value(id);
                    mapping.insert(id.value, client.isEmpty() ? allocateId() : newId(client));
                }
                for (auto i = c.clientIds.cbegin(); i != c.clientIds.cend(); ++i) if (!selected.contains(element(i.key()))) fail(ErrorCode::InvalidCommand, "Copy client identity is outside copied ownership");
                for (auto root : roots) {
                    const auto loc = impl_->elements.source.value(root); impl_->opaqueSafety(affected, true, &loc);
                    const auto &source = impl_->documents[loc.unit].document; const auto span = source.nodes()[loc.node].span;
                    for (auto i = impl_->nodeIds[loc.unit].cbegin(); i != impl_->nodeIds[loc.unit].cend(); ++i) {
                        const auto child = source.nodes()[i.key()].span;
                        if (child.offset < span.offset || child.offset + child.length > span.offset + span.length) continue;
                        if (!selected.contains(ElementId{i.value()}))
                            fail(ErrorCode::UnsafeRewrite, "Copy source ownership contains deleted/moved or unsupported descendants; save and reopen the changed source first");
                        if (i.value() != root.value) {
                            const auto current = s.elements.require(ElementId{i.value()});
                            if (current.owner != impl_->elements.original.value(current.id).owner)
                                fail(ErrorCode::UnsafeRewrite, "Copy source descendant ownership has changed; save and reopen first");
                        }
                    }
                    s.diagrams.each([&](const DiagramRecord &d) { if (selected.contains(d.owner)) fail(ErrorCode::UnsafeRewrite, "Copy with owned diagrams requires presentation cloning rules"); });
                    s.relations.each([&](const RelationRecord &r) { if (selected.contains(r.owner)) fail(ErrorCode::UnsafeRewrite, "Copy with owned relationships requires relationship identity allocation"); });
                }
                const auto destination = s.elements.require(element(c.owner));
                for (auto id : selected) {
                    const auto sourceLoc = impl_->elements.source.value(id);
                    if (impl_->documents[sourceLoc.unit].document.encoding() != impl_->unit(destination.unit).document.encoding())
                        fail(ErrorCode::UnsafeRewrite, "Source-preserving copy across different encodings is unsafe");
                    if (impl_->lineEndings[sourceLoc.unit] != impl_->lineEndings[impl_->units.value(destination.unit)])
                        fail(ErrorCode::UnsafeRewrite, "Source-preserving copy across different newline styles is unsafe");
                    auto r = s.elements.require(id); const auto source = r.id.value;
                    const ElementId copiedId{mapping.value(source)};
                    // Typed spellings are refreshed after the batch, so their baseline is
                    // the live source path, not a source name already renamed in this batch.
                    const auto baseline = impl_->elements.live.constFind(id);
                    copiedBaselines.insert(copiedId, baseline == impl_->elements.live.cend() ? r : baseline.value());
                    copiedSourcePaths.insert(copiedId, baseline == impl_->elements.live.cend()
                        ? qualified(s, id) : qualified(impl_->state(), id));
                    r.id = ElementId{mapping.value(source)}; r.owner = selected.contains(r.owner) ? ElementId{mapping.value(r.owner.value)} : destination.id; r.unit = destination.unit; r.readOnly = false;
                    r.properties["quid"] = r.id.value;
                    if (mapping.contains(r.properties.value("quidu"))) r.properties["quidu"] = mapping.value(r.properties.value("quidu"));
                    s.elements.put(r); delta.sourceCopies.insert(r.id.value, {source, mapping});
                }
            } else if constexpr (std::is_same_v<T, CreateRelation>) {
                if (c.kind != "Association" && c.kind != "Inheritance_Relationship") fail(ErrorCode::UnsupportedProfile, "Unsupported class relationship kind: " + c.kind);
                const auto owner = ownerFor(c.owner, c.kind, {});
                RelationRecord r; r.id = RelationId{newId(c.clientId)}; r.kind = c.kind; r.name = c.name; r.owner = owner.id; r.unit = owner.unit;
                for (auto id : c.endpoints) r.endpoints.append(element(id));
                r.properties["quid"] = r.id.value; r.propertyTypes["quid"] = "string";
                if (r.kind == "Association") for (int role = 0; role < 2; ++role) {
                    const auto key = QString("roles.%1.quid").arg(role);
                    r.properties[key] = allocateId(); r.propertyTypes[key] = "string";
                }
                s.relations.put(r);
                for (auto i = c.properties.cbegin(); i != c.properties.cend(); ++i) setProperty(s.relations, r.id, SetProperty{r.id, i.key(), i.value()});
            } else if constexpr (std::is_same_v<T, ReconnectRelation>) {
                auto r = s.relations.require(relation(c.id)); writable(r.readOnly, r.id.value); r.endpoints.clear(); for (auto id : c.endpoints) r.endpoints.append(element(id));
                if (r.kind == "Inheritance_Relationship" && !r.endpoints.isEmpty() && r.endpoints.front() != r.owner) {
                    const auto owner = ownerFor(r.endpoints.front(), r.kind, r.unit); r.owner = owner.id;
                }
                s.relations.put(r);
            } else if constexpr (std::is_same_v<T, CreateDiagram>) {
                if (c.kind != "ClassDiagram") fail(ErrorCode::UnsupportedProfile, "Only native ClassDiagram creation is supported");
                const auto owner = ownerFor(c.owner, c.kind, c.unit);
                DiagramRecord d{DiagramId{newId(c.clientId)}, c.kind, c.name, owner.id, owner.unit, {}, false}; s.diagrams.put(d);
            } else if constexpr (std::is_same_v<T, AddPresentation>) {
                auto d = s.diagrams.require(diagram(c.diagram)); writable(d.readOnly, d.id.value);
                if (d.kind != "ClassDiagram") fail(ErrorCode::UnsupportedProfile, "Only ClassDiagram presentation creation is supported");
                PresentationRecord p; p.diagram = d.id; p.unit = d.unit; p.geometry = c.geometry; validGeometry(p.geometry);
                quint64 label = 0; s.presentations.each([&](const PresentationRecord &r) {
                    if (r.diagram != d.id) return;
                    const bool authoredAssociation = r.kind == "AssociationViewNew" && !impl_->presentations.source.contains(r.id);
                    label = std::max(label, r.localLabel + (authoredAssociation ? 2 : 0));
                });
                // Include opaque native labels so insertion never shadows an unsupported view.
                const auto loc = impl_->diagrams.source.constFind(d.id);
                if (loc != impl_->diagrams.source.cend()) {
                    const auto &source = impl_->documents[loc->unit].document; const auto span = source.nodes()[loc->node].span;
                    for (const auto &node : source.nodes()) if (node.span.offset >= span.offset && node.span.offset + node.span.length <= span.offset + span.length)
                        for (qsizetype i = 2; i < node.headerParts; ++i) if (node.parts[i].kind == AtomKind::Reference) label = std::max(label, token(source, node.parts[i]).mid(1).toULongLong());
                }
                if (label == std::numeric_limits<quint64>::max()) fail(ErrorCode::InvalidCommand, "Diagram label space exhausted");
                p.localLabel = label + 1;
                std::visit([&](auto id) {
                    using I = decltype(id); id.value = resolve(id.value);
                    if constexpr (std::is_same_v<I, ElementId>) {
                        auto subject = s.elements.require(id); p.element = id; p.label = subject.name;
                        if (subject.kind == "Class") {
                            p.kind = "ClassView";
                            for (const auto &key : {QString("IncludeAttribute"), QString("IncludeOperation"), QString("ShowCompartmentStereotypes")}) {
                                p.properties[key] = "TRUE"; p.propertyTypes[key] = "boolean";
                            }
                        }
                        else if (subject.kind == "Class_Category") p.kind = "CategoryView";
                        else fail(ErrorCode::UnsupportedProfile, "Subject has no supported class-diagram view kind");
                    } else if constexpr (std::is_same_v<I, RelationId>) {
                        const auto subject = s.relations.require(id);
                        if (subject.kind != "Inheritance_Relationship" && subject.kind != "Association") fail(ErrorCode::UnsupportedProfile, "Unsupported native class relation presentation");
                        if (subject.endpoints.size() != 2) fail(ErrorCode::InvalidCommand, "Class relationship requires two endpoints");
                        p.relation = id; p.kind = subject.kind == "Association" ? "AssociationViewNew" : "InheritView";
                        if (p.kind == "AssociationViewNew" && p.localLabel > std::numeric_limits<quint64>::max() - 2)
                            fail(ErrorCode::InvalidCommand, "Association RoleView label space exhausted");
                        p.label = subject.name;
                        for (int end = 0; end < 2; ++end) {
                            PresentationId target; s.presentations.each([&](const PresentationRecord &r) {
                                if (r.diagram != d.id || r.element != subject.endpoints[end]) return;
                                if (!target.isEmpty()) fail(ErrorCode::UnsafeRewrite, "Relationship endpoint has ambiguous presentations in this diagram");
                                target = r.id;
                            });
                            if (target.isEmpty()) fail(ErrorCode::InvalidCommand, "Relationship endpoint must have a presentation in this diagram");
                            if (end == 0) p.client = target; else p.supplier = target;
                        }
                        const auto clientGeometry = s.presentations.require(p.client).geometry;
                        const auto supplierGeometry = s.presentations.require(p.supplier).geometry;
                        p.route = {{clientGeometry.x, clientGeometry.y}, {supplierGeometry.x, supplierGeometry.y}};
                        if (p.kind == "AssociationViewNew") p.route.insert(1, roleRoute(p.route, 0).front());
                        // Authored default routes use actual native box attachments,
                        // not centers that only the desktop renderer used to clip.
                        p.route.front() = boundaryAttachment(clientGeometry, p.route[1]);
                        p.route.back() = boundaryAttachment(supplierGeometry, p.route[p.route.size() - 2]);
                    } else fail(ErrorCode::UnsupportedProfile, "Presentation subject must be a class, package or supported class relationship");
                }, c.subject);
                p.id = PresentationId{newId(c.clientId, presentationId(p.unit, p.diagram, p.localLabel))};
                if (!p.relation.isEmpty()) authoredRouteGeometry.insert(p.id,
                    {s.presentations.require(p.client).geometry, s.presentations.require(p.supplier).geometry});
                s.presentations.put(p); d.presentations.append(p.id); s.diagrams.put(d);
            } else if constexpr (std::is_same_v<T, SetGeometry>) {
                auto p = s.presentations.require(presentation(c.id)); writable(p.readOnly, p.id.value);
                if (p.kind != "ClassView" && p.kind != "CategoryView") fail(ErrorCode::UnsupportedProfile, "Relationship geometry is expressed by its route");
                validGeometry(c.geometry); p.geometry = c.geometry; s.presentations.put(p);
            } else if constexpr (std::is_same_v<T, SetRoute>) {
                auto p = s.presentations.require(presentation(c.id)); writable(p.readOnly, p.id.value);
                if (p.kind != "InheritView" && p.kind != "AssociationViewNew") fail(ErrorCode::UnsupportedProfile, "Only native class relationship views have a supported route profile");
                if (c.points.size() < 2) fail(ErrorCode::InvalidCommand, "Relationship route requires at least two points");
                p.route = c.points;
                if (p.kind == "AssociationViewNew" && p.route.size() == 2) p.route.insert(1, roleRoute(p.route, 0).front());
                explicitRoutes.insert(p.id);
                s.presentations.put(p);
            } else if constexpr (std::is_same_v<T, SetMessageOrder>) {
                fail(ErrorCode::UnsupportedProfile, "Sequence message ordering is outside the native class profile");
            } else if constexpr (std::is_same_v<T, RemovePresentation>) {
                removePresentation(presentation(c.id));
            } else if constexpr (std::is_same_v<T, DeleteElement>) {
                const auto root = s.elements.require(element(c.id)); writable(root.readOnly, root.id.value);
                if (root.owner.isEmpty()) fail(ErrorCode::UnsafeRewrite, "Native root cannot be deep-deleted");
                const auto rootSource = impl_->elements.source.constFind(root.id);
                if (rootSource != impl_->elements.source.cend()) {
                    const auto &d = impl_->documents[rootSource->unit].document;
                    const int parent = d.nodes()[rootSource->node].parent;
                    if (parent < 0 || d.nodes()[parent].parts.isEmpty() || d.raw(d.nodes()[parent].parts[0].span) != "list")
                        fail(ErrorCode::UnsafeRewrite, "Native root/property-owned object cannot be deep-deleted");
                }
                QSet<ElementId> doomed{root.id}; bool expanded = true;
                while (expanded) { expanded = false; s.elements.each([&](const ElementRecord &r) { if (doomed.contains(r.owner) && !doomed.contains(r.id)) { doomed.insert(r.id); expanded = true; } }); }
                QSet<RelationId> doomedRelations; QSet<DiagramId> doomedDiagrams; QSet<PresentationId> doomedViews;
                s.relations.each([&](const RelationRecord &r) { bool hit = doomed.contains(r.owner); for (auto id : r.endpoints) hit = hit || doomed.contains(id); if (hit) doomedRelations.insert(r.id); });
                s.diagrams.each([&](const DiagramRecord &d) { if (doomed.contains(d.owner)) doomedDiagrams.insert(d.id); });
                s.presentations.each([&](const PresentationRecord &p) { if (doomed.contains(p.element) || doomedRelations.contains(p.relation) || doomedDiagrams.contains(p.diagram)) doomedViews.insert(p.id); });
                QSet<QString> dependents, affected;
                for (auto id : doomed) { affected.insert(id.value); if (id != root.id) dependents.insert(id.value); }
                for (auto id : doomedRelations) {
                    affected.insert(id.value); dependents.insert(id.value);
                    const auto r = s.relations.require(id);
                    if (r.kind == "Association") for (int role = 0; role < 2; ++role) {
                        const auto roleId = r.properties.value(QString("roles.%1.quid").arg(role));
                        affected.insert(roleId); deletedRoleIds.insert(roleId);
                    }
                }
                for (auto id : doomedDiagrams) { affected.insert(id.value); dependents.insert(id.value); }
                for (auto id : doomedViews) dependents.insert(id.value);
                QSet<QString> acknowledged; for (auto id : c.acknowledgedDependents) acknowledged.insert(resolve(id));
                if (acknowledged != dependents) fail(ErrorCode::InvalidCommand, "Deep delete requires exact acknowledgement of owned elements, incident relationships, diagrams and presentations");
                impl_->opaqueSafety(affected, false);
                // Known scalar element references are dependencies, not strings to silently erase.
                s.elements.each([&](const ElementRecord &r) { if (!doomed.contains(r.id) && affected.contains(r.properties.value("quidu"))) fail(ErrorCode::UnsafeRewrite, "Surviving typed property references deleted identity"); });
                for (auto id : doomedViews) removePresentation(id, doomedViews);
                for (auto id : doomedDiagrams) { writable(s.diagrams.require(id).readOnly, id.value); s.diagrams.remove(id); }
                for (auto id : doomedRelations) { writable(s.relations.require(id).readOnly, id.value); s.relations.remove(id); }
                for (auto id : doomed) { writable(s.elements.require(id).readOnly, id.value); s.elements.remove(id); }
            }
        }, command.payload);
        // Resolve reconnects in each diagram, then carry native endpoint attachments
        // with changed class appearances. Interior bends stay exactly as authored.
        s.presentations.each([&](const PresentationRecord &p) {
            if (p.kind != "InheritView" && p.kind != "AssociationViewNew") return;
            const auto *r = s.relations.get(p.relation);
            if (!r || r->endpoints.size() != 2) return;
            auto updated = p;
            for (int end = 0; end < 2; ++end) {
                auto &endpoint = end == 0 ? updated.client : updated.supplier;
                const auto *current = s.presentations.get(endpoint);
                if (current && current->diagram == p.diagram && current->element == r->endpoints[end]) continue;
                PresentationId target;
                s.presentations.each([&](const PresentationRecord &candidate) {
                    if (candidate.diagram != p.diagram || candidate.element != r->endpoints[end]) return;
                    if (!target.isEmpty()) fail(ErrorCode::UnsafeRewrite, "Relationship endpoint has ambiguous presentations in this diagram");
                    target = candidate.id;
                });
                if (target.isEmpty()) fail(ErrorCode::UnsafeRewrite, "Relationship endpoint has no presentation in this diagram");
                endpoint = target;
            }
            if (!explicitRoutes.contains(p.id) && !updated.route.isEmpty()) {
                for (int end = 0; end < 2; ++end) {
                    const auto oldId = end == 0 ? p.client : p.supplier;
                    const auto newId = end == 0 ? updated.client : updated.supplier;
                    const auto old = impl_->presentations.live.constFind(oldId);
                    const auto *stagedOld = s.presentations.get(oldId);
                    const auto *target = s.presentations.get(newId);
                    const auto *before = old != impl_->presentations.live.cend() ? &old.value() : stagedOld;
                    const auto authored = authoredRouteGeometry.constFind(p.id);
                    if (!target || (!before && authored == authoredRouteGeometry.cend())) continue;
                    const auto &geometry = authored != authoredRouteGeometry.cend()
                        ? (end == 0 ? authored->first : authored->second) : before->geometry;
                    if (oldId == newId && geometry == target->geometry) continue;
                    auto &attachment = end == 0 ? updated.route.front() : updated.route.back();
                    attachment = reattach(attachment, geometry, target->geometry);
                }
            }
            if (updated.client != p.client || updated.supplier != p.supplier || updated.route != p.route) {
                writable(p.readOnly, p.id.value); s.presentations.put(updated);
            }
        });
        impl_->validate(s);
        // Name/owner changes refresh every appearance, including descendant qualified names.
        s.presentations.each([&](const PresentationRecord &p) {
            if (p.element.isEmpty()) return;
            const auto *subject = s.elements.get(p.element); if (!subject) return;
            const auto *old = impl_->elements.live.constFind(p.element) == impl_->elements.live.cend() ? nullptr : &impl_->elements.live.constFind(p.element).value();
            const bool changedPath = !old || qualified(s, p.element) != qualified(impl_->state(), p.element);
            if (changedPath || (old && old->name != subject->name)) {
                writable(p.readOnly, p.id.value); auto changed = p;
                if (!old || old->name != subject->name) changed.label = subject->name;
                s.presentations.put(changed);
            }
        });
        // Relationship qualified source spellings may lie in a read-only unit.
        s.relations.each([&](const RelationRecord &r) {
            const auto existing = impl_->relations.live.constFind(r.id);
            for (auto id : r.endpoints) {
                const bool changedPath = impl_->elements.live.contains(id) && qualified(s, id) != qualified(impl_->state(), id);
                if (existing == impl_->relations.live.cend() || existing->endpoints != r.endpoints || changedPath)
                    quote(qualified(s, id), impl_->unit(r.unit).document.encoding());
                if (changedPath) writable(r.readOnly, r.id.value);
            }
            if (r.kind == "Inheritance_Relationship") {
                const auto prior = impl_->relations.live.constFind(r.id);
                const bool changed = prior == impl_->relations.live.cend() || prior->endpoints != r.endpoints
                    || qualified(s, r.endpoints[1]) != qualified(impl_->state(), r.endpoints[1]);
                if (changed) {
                    writable(r.readOnly, r.id.value); auto updated = r;
                    updated.properties["quidu"] = r.endpoints[1].value; updated.propertyTypes["quidu"] = "string";
                    updated.properties["supplier"] = qualified(s, r.endpoints[1]); updated.propertyTypes["supplier"] = "string";
                    s.relations.put(updated);
                }
            }
        });
        s.elements.each([&](const ElementRecord &r) {
            const ElementId target{r.properties.value("quidu")};
            if (target.isEmpty()) return;
            const auto *after = s.elements.get(target);
            const ElementRecord *before = nullptr;
            QString oldPath;
            const auto prior = impl_->elements.live.constFind(target);
            if (prior != impl_->elements.live.cend()) {
                before = &prior.value(); oldPath = qualified(impl_->state(), target);
            } else {
                const auto copied = copiedBaselines.constFind(target);
                if (copied == copiedBaselines.cend()) return;
                before = &copied.value(); oldPath = copiedSourcePaths.value(target);
            }
            const auto newPath = qualified(s, target);
            const bool targetChanged = after->name != before->name || oldPath != newPath;
            QString key;
            if (r.kind == "ClassAttribute" || r.kind == "Parameter") key = "type";
            else if (r.kind == "Operation") key = "result";
            else if (r.kind == "Class_Category") key = "subsystem";
            else if (r.kind == "SubSystem") key = "category";
            if (key.isEmpty() || !r.properties.contains(key)) return;
            const auto text = r.properties.value(key);
            const auto existingReference = impl_->elements.live.constFind(r.id);
            const auto copiedReference = copiedBaselines.constFind(r.id);
            const ElementRecord *referenceBefore = existingReference != impl_->elements.live.cend()
                ? &existingReference.value()
                : copiedReference != copiedBaselines.cend() ? &copiedReference.value() : nullptr;
            const bool spellingChanged = referenceBefore && referenceBefore->properties.value(key) != text;
            if (spellingChanged && (text == newPath || text == after->name)) return;
            if (!targetChanged) {
                if (spellingChanged)
                    fail(ErrorCode::UnsafeRewrite, "Scalar typed field cannot change its native reference binding: " + r.id.value + " / " + key);
                return;
            }
            QString replacement;
            if (text == oldPath) replacement = newPath;
            else if (text == before->name) replacement = after->name;
            else fail(ErrorCode::UnsafeRewrite, "Native typed reference spelling is ambiguous: " + r.id.value + " / " + key);
            if (text != replacement) {
                writable(r.readOnly, r.id.value); auto changed = r; changed.properties[key] = replacement; s.elements.put(changed);
            }
        });
        QSet<QString> affectedIdentities = deletedRoleIds;
        for (auto i = s.elements.edits.cbegin(); i != s.elements.edits.cend(); ++i) {
            const auto prior = impl_->elements.live.constFind(i.key());
            if (prior != impl_->elements.live.cend() && (!i.value() || i.value()->owner != prior->owner)) affectedIdentities.insert(i.key().value);
        }
        for (auto i = s.relations.edits.cbegin(); i != s.relations.edits.cend(); ++i) if (!i.value()) {
            affectedIdentities.insert(i.key().value);
            const auto prior = impl_->relations.live.constFind(i.key());
            if (prior != impl_->relations.live.cend() && prior->kind == "Association")
                for (int role = 0; role < 2; ++role)
                    affectedIdentities.insert(prior->properties.value(QString("roles.%1.quid").arg(role)));
        }
        auto checkOpaqueScalars = [&](const auto &record, const QSet<QString> &ids) {
            for (auto p = record.properties.cbegin(); p != record.properties.cend(); ++p) {
                if (p.key() == "quid" || p.key() == "quidu" || p.key() == "client" || p.key() == "supplier" || p.key() == "Parent_View") continue;
                for (const auto &id : ids) if (p.value().contains(id)) fail(ErrorCode::UnsafeRewrite, "Scalar property may hide a reference to affected identity: " + record.id.value + " / " + p.key());
            }
        };
        if (!affectedIdentities.isEmpty()) {
            s.elements.each([&](const ElementRecord &r) { checkOpaqueScalars(r, affectedIdentities); });
            s.relations.each([&](const RelationRecord &r) { checkOpaqueScalars(r, affectedIdentities); });
            s.presentations.each([&](const PresentationRecord &r) { checkOpaqueScalars(r, affectedIdentities); });
        }
        for (const auto &copy : delta.sourceCopies) {
            const auto *source = s.elements.get(ElementId{copy.sourceId}); if (!source) continue;
            QSet<QString> ids; for (auto i = copy.ids.cbegin(); i != copy.ids.cend(); ++i) ids.insert(i.key());
            checkOpaqueScalars(*source, ids);
        }
        impl_->preflight(s);
        delta.elements = changes(s.elements); delta.relations = changes(s.relations); delta.diagrams = changes(s.diagrams); delta.presentations = changes(s.presentations);
        auto describe = [&](const auto &list, const QString &kind) {
            for (const auto &c : list) {
                const auto &r = c.after ? *c.after : *c.before;
                if (!c.before || !c.after) delta.diff.append({kind, r.id.value, c.after ? "create" : "delete", c.before ? r.kind : QString{}, c.after ? r.kind : QString{}});
                if (!c.after) continue;
                using R = std::decay_t<decltype(r)>;
                const R empty;
                const auto &before = c.before ? *c.before : empty;
                const auto &after = *c.after;
                auto field = [&](const QString &key, const QString &oldValue, const QString &newValue) {
                    if (!c.before || oldValue != newValue) delta.diff.append({kind, r.id.value, key, c.before ? oldValue : QString{}, newValue});
                };
                field("unit", before.unit.value, after.unit.value);
                field("readOnly", before.readOnly ? "TRUE" : "FALSE", after.readOnly ? "TRUE" : "FALSE");
                if constexpr (!std::is_same_v<R, PresentationRecord>) {
                    field("name", before.name, after.name);
                    field("owner", before.owner.value, after.owner.value);
                } else {
                    auto geometryText = [](const Geometry &g) { return QString::fromLatin1(number(g.x) + ',' + number(g.y) + ',' + number(g.width) + ',' + number(g.height)); };
                    auto routeText = [](const QVector<WorldPoint> &route) { QByteArray out; for (const auto &p : route) { if (!out.isEmpty()) out += ' '; out += point(p); } return QString::fromLatin1(out); };
                    field("geometry", geometryText(before.geometry), geometryText(after.geometry));
                    field("label", before.label, after.label);
                    field("route", routeText(before.route), routeText(after.route));
                    field("diagram", before.diagram.value, after.diagram.value);
                    field("element", before.element.value, after.element.value);
                    field("relation", before.relation.value, after.relation.value);
                    field("parent", before.parent.value, after.parent.value);
                    field("client", before.client.value, after.client.value);
                    field("supplier", before.supplier.value, after.supplier.value);
                    field("localLabel", QString::number(before.localLabel), QString::number(after.localLabel));
                }
                if constexpr (!std::is_same_v<R, DiagramRecord>) {
                    for (auto i = after.properties.cbegin(); i != after.properties.cend(); ++i) field(i.key(), before.properties.value(i.key()), i.value());
                    for (auto i = after.propertyTypes.cbegin(); i != after.propertyTypes.cend(); ++i) field(i.key() + ".type", before.propertyTypes.value(i.key()), i.value());
                }
                if constexpr (std::is_same_v<R, RelationRecord>) {
                    QStringList oldEnds, newEnds;
                    for (auto id : before.endpoints) oldEnds.append(id.value);
                    for (auto id : after.endpoints) newEnds.append(id.value);
                    field("endpoints", oldEnds.join(","), newEnds.join(","));
                }
                if constexpr (std::is_same_v<R, DiagramRecord>) {
                    QStringList oldViews, newViews;
                    for (auto id : before.presentations) oldViews.append(id.value);
                    for (auto id : after.presentations) newViews.append(id.value);
                    field("presentations", oldViews.join(","), newViews.join(","));
                }
            }
        };
        describe(delta.elements, "element"); describe(delta.relations, "relation"); describe(delta.diagrams, "diagram"); describe(delta.presentations, "presentation");
        return delta;
    } catch (const WorkspaceError &e) { return e; }
}

Status Model::applyDelta(const ModelDelta &delta, bool forward) {
    try {
        auto s = impl_->state();
        loadChanges(s.elements, delta.elements, forward); loadChanges(s.relations, delta.relations, forward);
        loadChanges(s.diagrams, delta.diagrams, forward); loadChanges(s.presentations, delta.presentations, forward);
        impl_->validate(s); impl_->preflight(s);
        commit(impl_->elements, s.elements); commit(impl_->relations, s.relations); commit(impl_->diagrams, s.diagrams); commit(impl_->presentations, s.presentations);
        if (forward) for (auto i = delta.sourceCopies.cbegin(); i != delta.sourceCopies.cend(); ++i) impl_->copies.insert(i.key(), i.value());
        else for (auto i = delta.sourceCopies.cbegin(); i != delta.sourceCopies.cend(); ++i) impl_->copies.remove(i.key());
        auto reserve = [&](const auto &overlay) {
            for (const auto &r : overlay.edits) if (r) {
                impl_->nativeIds.insert(r->id.value);
                using R = std::decay_t<decltype(*r)>;
                if constexpr (std::is_same_v<R, RelationRecord>) {
                    if (r->kind == "Association") for (int role = 0; role < 2; ++role)
                        impl_->nativeIds.insert(r->properties.value(QString("roles.%1.quid").arg(role)));
                }
            }
        };
        reserve(s.elements); reserve(s.relations); reserve(s.diagrams);
        return std::monostate{};
    } catch (const WorkspaceError &e) { return e; }
}

Outcome<QMap<UnitId, QByteArray>> Model::Impl::serialize(const State &s, const QMap<QString, SourceCopy> &sourceCopies) const {
    try {
        validate(s);
        struct Object { QString id, kind, owner; UnitId unit; Location location; bool copied = false; };
        QMap<QString, Object> objects;
        auto collect = [&](const auto &overlay, const auto &sources) {
            overlay.each([&](const auto &r) {
                using R = std::decay_t<decltype(r)>;
                QString owner;
                if constexpr (std::is_same_v<R, PresentationRecord>) owner = r.diagram.value; else owner = r.owner.value;
                Location loc; bool copied = false;
                auto original = sources.constFind(r.id);
                if (original != sources.cend()) loc = original.value();
                else {
                    auto copy = sourceCopies.constFind(r.id.value);
                    if (copy != sourceCopies.cend()) {
                        using I = decltype(r.id);
                        const auto origin = sources.constFind(I{copy->sourceId});
                        if (origin == sources.cend()) fail(ErrorCode::UnsafeRewrite, "Copy source identity is unavailable");
                        loc = origin.value(); copied = true;
                    }
                }
                objects.insert(r.id.value, {r.id.value, r.kind, owner, r.unit, loc, copied});
            });
        };
        collect(s.elements, elements.source); collect(s.relations, relations.source); collect(s.diagrams, diagrams.source); collect(s.presentations, presentations.source);
        QMap<QString, QVector<QString>> children;
        for (const auto &o : objects) if (!o.owner.isEmpty()) children[o.owner].append(o.id);
        using Patch = std::pair<SourceSpan, QByteArray>;
        QVector<QVector<Patch>> scalar(documents.size()), removals(documents.size()), additions(documents.size());
        auto append = [&](QVector<Patch> &out, SourceSpan span, QByteArray bytes, const PetalDocument &d) {
            if (d.raw(span) != ByteView(bytes)) out.append({span, std::move(bytes)});
        };
        auto slice = [&](const PetalDocument &d, SourceSpan span, QVector<Patch> patches) {
            std::stable_sort(patches.begin(), patches.end(), [](const auto &a, const auto &b) { return a.first.offset < b.first.offset || (a.first.offset == b.first.offset && a.first.length > b.first.length); });
            QByteArray out; qsizetype cursor = span.offset;
            const Patch *previous = nullptr;
            QMap<qsizetype, QSet<QByteArray>> insertions;
            for (const auto &p : patches) {
                if (p.first.offset < span.offset || p.first.offset + p.first.length > span.offset + span.length) continue;
                if (p.first.length == 0) {
                    auto &at = insertions[p.first.offset]; if (at.contains(p.second)) continue; at.insert(p.second);
                }
                if (p.first.offset < cursor) {
                    if (previous && previous->first.offset == p.first.offset && previous->first.length == p.first.length && previous->second == p.second) continue;
                    if (previous && previous->second.isEmpty() && p.first.offset + p.first.length <= cursor) continue;
                    fail(ErrorCode::UnsafeRewrite, "Native source patches overlap");
                }
                out.append(d.bytes().constData() + cursor, p.first.offset - cursor); out += p.second;
                cursor = p.first.offset + p.first.length;
                previous = &p;
            }
            out.append(d.bytes().constData() + cursor, span.offset + span.length - cursor); return out;
        };
        auto scalarEdits = [&](const Object &o, QVector<Patch> &out) {
            if (o.location.node < 0) return;
            const auto &d = documents[o.location.unit].document; const auto &node = d.nodes()[o.location.node];
            const auto &encoding = unit(o.unit).document.encoding();
            auto editProperty = [&](int n, const QString &key, QByteArray value) {
                const auto &target = d.nodes()[n]; bool found = false;
                for (const auto &p : target.properties) if (d.raw(p.key) == key.toLatin1()) {
                    if (found) fail(ErrorCode::UnsafeRewrite, "Ambiguous duplicate property: " + key);
                    append(out, p.value.span, value, d); found = true;
                }
                if (!found) out.append({{target.span.offset + target.span.length - 1, 0}, ' ' + key.toLatin1() + ' ' + value});
            };
            auto editName = [&](const QString &name) {
                if (node.headerParts > 2 && node.parts[2].kind == AtomKind::Quoted) {
                    if (token(d, node.parts[2]) != name) append(out, node.parts[2].span, quote(name, encoding), d);
                }
                else if (!name.isEmpty()) fail(ErrorCode::UnsafeRewrite, "Native unnamed object cannot acquire a positional name");
            };
            auto editProperties = [&](const auto &record) {
                for (auto i = record.properties.cbegin(); i != record.properties.cend(); ++i) {
                    if (i.key().startsWith("roles.")) continue; // Read-only nested Role identity metadata.
                    const auto type = record.propertyTypes.value(i.key());
                    if (type == "opaque") continue;
                    QString existing = property(d, o.location.node, i.key());
                    if (existing != i.value()) editProperty(o.location.node, i.key(), encode(i.value(), type, encoding));
                }
            };
            if (const auto *r = s.elements.get(ElementId{o.id})) { editName(r->name); editProperties(*r); }
            else if (const auto *r = s.relations.get(RelationId{o.id})) {
                editName(r->name); editProperties(*r);
                auto changedEndpoint = [&](int index) {
                    const auto prior = relations.original.constFind(r->id);
                    if (prior == relations.original.cend() || prior->endpoints[index] != r->endpoints[index]) return true;
                    return qualified(s, r->endpoints[index]) != qualified(originalState(), r->endpoints[index]);
                };
                if (r->kind == "Inheritance_Relationship") {
                    if (changedEndpoint(1)) {
                        editProperty(o.location.node, "quidu", quote(r->endpoints[1].value, encoding));
                        editProperty(o.location.node, "supplier", quote(qualified(s, r->endpoints[1]), encoding));
                    }
                } else {
                    int index = 0;
                    for (const auto &p : node.properties) if (d.raw(p.key) == "roles" && p.value.child >= 0)
                        for (const auto &role : d.nodes()[p.value.child].parts) if (role.child >= 0 && kind(d, role.child) == "Role") {
                            if (index >= r->endpoints.size()) fail(ErrorCode::UnsafeRewrite, "Association role count changed");
                            if (changedEndpoint(index)) {
                                editProperty(role.child, "quidu", quote(r->endpoints[index].value, encoding));
                                editProperty(role.child, "supplier", quote(qualified(s, r->endpoints[index]), encoding));
                            }
                            ++index;
                        }
                    if (index != 2) fail(ErrorCode::UnsafeRewrite, "Association roles are not binary");
                }
            } else if (const auto *r = s.diagrams.get(DiagramId{o.id})) {
                editName(r->name);
                if (r->name != diagrams.original.value(r->id).name) editProperty(o.location.node, "title", quote(r->name, encoding));
            } else if (const auto *r = s.presentations.get(PresentationId{o.id})) {
                editProperties(*r);
                if (!r->element.isEmpty()) {
                    const qsizetype namePart = r->kind == "ClassView" ? 3 : 2;
                    if (node.headerParts <= namePart || node.parts[namePart].kind != AtomKind::Quoted) fail(ErrorCode::UnsafeRewrite, "Presentation qualified header is unavailable");
                    const auto &old = presentations.original.value(r->id);
                    if (qualified(s, r->element) != qualified(originalState(), r->element))
                        append(out, node.parts[namePart].span, quote(qualified(s, r->element), encoding), d);
                    if (r->label != old.label)
                        for (const auto &p : node.properties) if (d.raw(p.key) == "label" && p.value.child >= 0) editProperty(p.value.child, "label", quote(r->label, encoding));
                    if (r->geometry != old.geometry) {
                        editProperty(o.location.node, "location", point({r->geometry.x, r->geometry.y}));
                        editProperty(o.location.node, "width", number(r->geometry.width)); editProperty(o.location.node, "height", number(r->geometry.height));
                        for (const auto &label : node.properties) if (d.raw(label.key) == "label" && label.value.child >= 0) {
                            for (const auto &field : d.nodes()[label.value.child].properties) if (d.raw(field.key) == "location") {
                                const auto oldPoint = readPoint(d, field.value);
                                editProperty(label.value.child, "location", point({
                                    oldPoint.x + r->geometry.x - old.geometry.x - (r->geometry.width - old.geometry.width) / 2,
                                    oldPoint.y + r->geometry.y - old.geometry.y - (r->geometry.height - old.geometry.height) / 2}));
                            }
                        }
                    }
                } else if (r->kind == "AssociationViewNew") {
                    const auto roles = associationRoles(d, o.location.node);
                    const auto relation = s.relations.require(r->relation);
                    const auto old = presentations.original.value(r->id);
                    for (int end = 0; end < 2; ++end) {
                        int role = -1;
                        for (int candidate : roles)
                            if (property(d, candidate, "quidu") == relation.properties.value(QString("roles.%1.quid").arg(end))) role = candidate;
                        if (role < 0) fail(ErrorCode::DanglingReference, "Missing native RoleView identity");
                        const auto target = s.presentations.require(end == 0 ? r->client : r->supplier);
                        editProperty(role, "supplier", '@' + QByteArray::number(target.localLabel));
                        if (r->route != old.route) {
                            const auto points = roleRoute(r->route, end);
                            QByteArray vertices("(list Points"); for (const auto &p : points) vertices += ' ' + point(p); vertices += ')';
                            editProperty(role, "vertices", vertices);
                            editProperty(role, "origin_attachment", point(points.front()));
                            editProperty(role, "terminal_attachment", point(points.back()));
                        }
                    }
                    if (r->route != old.route) editProperty(o.location.node, "location", point(roleRoute(r->route, 0).front()));
                } else {
                    auto local = [&](PresentationId id) { const auto *p = s.presentations.get(id); if (!p) fail(ErrorCode::DanglingReference, "Missing route endpoint"); return '@' + QByteArray::number(p->localLabel); };
                    editProperty(o.location.node, "client", local(r->client)); editProperty(o.location.node, "supplier", local(r->supplier));
                    if (r->route != presentations.original.value(r->id).route) {
                        QByteArray route("(list Points"); for (auto p : r->route) route += ' ' + point(p); route += ')';
                        editProperty(o.location.node, "vertices", route);
                        if (!r->route.isEmpty()) { editProperty(o.location.node, "origin_attachment", point(r->route.front())); editProperty(o.location.node, "terminal_attachment", point(r->route.back())); }
                    }
                }
            }
        };
        auto originalOwner = [&](const Object &o) {
            if (const auto i = elements.original.constFind(ElementId{o.id}); i != elements.original.cend()) return i->owner.value;
            if (const auto i = relations.original.constFind(RelationId{o.id}); i != relations.original.cend()) return i->owner.value;
            if (const auto i = diagrams.original.constFind(DiagramId{o.id}); i != diagrams.original.cend()) return i->owner.value;
            if (const auto i = presentations.original.constFind(PresentationId{o.id}); i != presentations.original.cend()) return i->diagram.value;
            return QString{};
        };
        QSet<QString> relocated;
        for (const auto &o : objects) {
            if (o.location.node >= 0 && !o.copied) {
                scalarEdits(o, scalar[o.location.unit]);
                if (o.owner != originalOwner(o)) relocated.insert(o.id);
            } else relocated.insert(o.id);
        }
        for (int u = 0; u < documents.size(); ++u) for (auto i = nodeIds[u].cbegin(); i != nodeIds[u].cend(); ++i) {
            const auto current = objects.constFind(i.value());
            const auto &d = documents[u].document;
            if (current == objects.cend() || (relocated.contains(i.value()) && current->location.unit == u && current->location.node == i.key()))
                removals[u].append({d.nodes()[i.key()].span, {}});
            else if (current->location.unit != u || current->location.node != i.key()) {
                // Stub spellings can differ from the loaded definition. Only a semantic rename changes them.
                const auto *r = s.elements.get(ElementId{i.value()});
                const auto baseline = elements.original.constFind(ElementId{i.value()});
                if (r && baseline != elements.original.cend() && baseline->name != r->name
                    && d.name(i.key()) && d.name(i.key())->text() != r->name && d.nodes()[i.key()].headerParts > 2)
                    append(scalar[u], d.nodes()[i.key()].parts[2].span, quote(r->name, d.encoding()), d);
            }
        }
        // Rose resolves local view references while reading a diagram, not in a
        // second pass like our projection. Order authored siblings by dependency;
        // neither QSet iteration nor lexical IDs (@10 before @2) is native order.
        QMap<QString, qsizetype> viewOrder;
        for (auto siblings = children.begin(); siblings != children.end(); ++siblings) {
            if (objects.value(siblings.key()).kind != "ClassDiagram") continue;
            bool authored = false;
            for (const auto &id : siblings.value()) if (relocated.contains(id)) { authored = true; break; }
            if (!authored) continue;
            auto &ids = siblings.value();
            std::sort(ids.begin(), ids.end(), [&](const QString &a, const QString &b) {
                return s.presentations.get(PresentationId{a})->localLabel
                    < s.presentations.get(PresentationId{b})->localLabel;
            });
            QVector<QString> ordered;
            QSet<QString> visiting, visited;
            std::function<void(const QString &)> visit = [&](const QString &id) {
                if (visited.contains(id)) return;
                if (visiting.contains(id)) fail(ErrorCode::UnsafeRewrite, "Native view dependency cycle");
                visiting.insert(id);
                const auto &p = *s.presentations.get(PresentationId{id});
                for (const auto &dependency : {p.parent, p.client, p.supplier})
                    if (!dependency.isEmpty()) visit(dependency.value);
                visiting.remove(id); visited.insert(id);
                viewOrder.insert(id, ordered.size()); ordered.append(id);
            };
            for (const auto &id : ids) visit(id);
            ids = std::move(ordered);
        }
        // A reconnect can bind an existing route to a newly added class view.
        // Insert that declaration before its first existing consumer, retaining
        // all original item bytes/order rather than normalizing imported diagrams.
        QMap<QString, qsizetype> viewInsertion;
        if (!viewOrder.isEmpty()) s.presentations.each([&](const PresentationRecord &p) {
            if (relocated.contains(p.id.value)) return;
            const auto loc = presentations.source.constFind(p.id);
            if (loc == presentations.source.cend()) return;
            const auto offset = documents[loc->unit].document.nodes()[loc->node].span.offset;
            for (const auto &dependency : {p.parent, p.client, p.supplier}) {
                if (!relocated.contains(dependency.value)) continue;
                auto insertion = viewInsertion.find(dependency.value);
                if (insertion == viewInsertion.end()) viewInsertion.insert(dependency.value, offset);
                else insertion.value() = std::min(insertion.value(), offset);
            }
        });
        std::function<QByteArray(const QString &)> render;
        QMap<ElementId, QVector<ElementId>> sourceChildren;
        if (!sourceCopies.isEmpty()) for (auto i = elements.original.cbegin(); i != elements.original.cend(); ++i)
            sourceChildren[i->owner].append(i.key());
        QSet<QString> rendering;
        auto newProperties = [&](const auto &r, const QByteArray &encoding) {
            QByteArray out;
            for (auto i = r.properties.cbegin(); i != r.properties.cend(); ++i)
                if (i.key() != "quid" && i.key() != "quidu" && i.key() != "supplier" && !i.key().startsWith("roles.") && r.propertyTypes.value(i.key()) != "opaque")
                    out += ' ' + i.key().toLatin1() + ' ' + encode(i.value(), r.propertyTypes.value(i.key()), encoding);
            return out;
        };
        render = [&](const QString &id) -> QByteArray {
            if (rendering.contains(id)) fail(ErrorCode::InvalidCommand, "Native serialization ownership cycle");
            rendering.insert(id); const auto o = objects.value(id); const auto &encoding = unit(o.unit).document.encoding();
            QByteArray output;
            if (o.location.node >= 0) {
                const auto &d = documents[o.location.unit].document; const auto span = d.nodes()[o.location.node].span;
                QVector<Patch> patches;
                if (o.copied) {
                    const auto copy = sourceCopies.value(id);
                    scalarEdits(o, patches);
                    QSet<QString> retained;
                    // Replace only this copy's direct source children. Recursive rendering
                    // carries their current ownership edits without touching opaque siblings.
                    for (const auto &sourceChild : sourceChildren.value(ElementId{copy.sourceId})) {
                        const auto loc = elements.source.value(sourceChild, Location{});
                        if (loc.unit != o.location.unit || loc.node < 0) continue;
                        const auto childSpan = d.nodes()[loc.node].span;
                        if (childSpan.offset < span.offset || childSpan.offset + childSpan.length > span.offset + span.length) continue;
                        const auto childId = copy.ids.value(sourceChild.value);
                        const auto child = objects.constFind(childId);
                        if (child != objects.cend() && child->owner == id) {
                            patches.append({childSpan, render(childId)}); retained.insert(childId);
                        } else patches.append({childSpan, {}});
                    }
                    QMap<QString, QByteArray> inserted;
                    for (const auto &childId : children.value(id)) if (!retained.contains(childId)) {
                        const auto key = listKey(objects.value(childId).kind, o.kind);
                        if (key.isEmpty()) fail(ErrorCode::UnsupportedProfile, "No native owner-list serialization rule");
                        inserted[key] += lineEndings[units.value(o.unit)] + render(childId);
                    }
                    for (auto i = inserted.cbegin(); i != inserted.cend(); ++i) {
                        int list = -1;
                        for (const auto &p : d.nodes()[o.location.node].properties) if (d.raw(p.key) == i.key().toLatin1()) {
                            if (list >= 0 || p.value.child < 0 || d.nodes()[p.value.child].parts.isEmpty()
                                || d.raw(d.nodes()[p.value.child].parts[0].span) != "list")
                                fail(ErrorCode::UnsafeRewrite, "Owner list is ambiguous or not native");
                            list = p.value.child;
                        }
                        if (list >= 0) patches.append({{d.nodes()[list].span.offset + d.nodes()[list].span.length - 1, 0}, i.value()});
                        else patches.append({{span.offset + span.length - 1, 0},
                            ' ' + i.key().toLatin1() + " (list " + listType(i.key()).toLatin1() + i.value() + ')'});
                    }
                } else {
                    patches = scalar[o.location.unit];
                    for (const auto &p : removals[o.location.unit]) if (p.first.offset != span.offset) patches.append(p);
                    patches += additions[o.location.unit];
                }
                output = slice(d, span, std::move(patches));
            } else {
                if (const auto *r = s.elements.get(ElementId{id})) {
                    output = "(object " + r->kind.toLatin1() + ' ' + quote(r->name, encoding) + " quid " + quote(id, encoding) + newProperties(*r, encoding);
                } else if (const auto *r = s.relations.get(RelationId{id})) {
                    output = "(object " + r->kind.toLatin1();
                    if (r->kind == "Association") output += ' ' + quote(r->name, encoding);
                    else if (!r->name.isEmpty()) fail(ErrorCode::UnsupportedProfile, "Native Inheritance_Relationship has no name header");
                    output += " quid " + quote(id, encoding) + newProperties(*r, encoding);
                    if (r->kind == "Association") {
                        output += " roles (list role_list";
                        for (int end = 0; end < 2; ++end) {
                            const auto roleId = r->properties.value(QString("roles.%1.quid").arg(end));
                            if (roleId.isEmpty()) fail(ErrorCode::DanglingReference, "Association Role identity is missing");
                            output += " (object Role " + quote("$UNNAMED$" + QString::number(end), encoding) + " quid " + quote(roleId, encoding)
                                + " supplier " + quote(qualified(s, r->endpoints[end]), encoding) + " quidu " + quote(r->endpoints[end].value, encoding) + ')';
                        }
                        output += ')';
                    } else output += " supplier " + quote(qualified(s, r->endpoints[1]), encoding) + " quidu " + quote(r->endpoints[1].value, encoding);
                } else if (const auto *r = s.diagrams.get(DiagramId{id})) {
                    output = "(object ClassDiagram " + quote(r->name, encoding) + " quid " + quote(id, encoding) + " title " + quote(r->name, encoding) + " zoom 100 max_height 28350 max_width 21600 origin_x 0 origin_y 0";
                } else if (const auto *r = s.presentations.get(PresentationId{id})) {
                    output = "(object " + r->kind.toLatin1();
                    if (r->kind == "ClassView") output += " \"Class\"";
                    output += ' ' + quote(r->kind == "AssociationViewNew" ? r->label : r->element.isEmpty() ? QString{} : qualified(s, r->element), encoding) + " @" + QByteArray::number(r->localLabel);
                    output += " quidu " + quote(r->element.isEmpty() ? r->relation.value : r->element.value, encoding);
                    if (!r->element.isEmpty()) {
                        output += " location " + point({r->geometry.x, r->geometry.y}) + " width " + number(r->geometry.width) + " height " + number(r->geometry.height)
                            + " label (object ItemLabel Parent_View @" + QByteArray::number(r->localLabel)
                            + " location " + point({r->geometry.x - r->geometry.width / 2 + 10, r->geometry.y - r->geometry.height / 2 + 10})
                            + " nlines 1 max_width " + number(std::max(1.0, r->geometry.width - 20)) + " justify 0 label " + quote(r->label, encoding) + ')';
                    } else if (r->kind == "AssociationViewNew") {
                        const auto relation = s.relations.require(r->relation);
                        const auto junction = roleRoute(r->route, 0).front();
                        output += " location " + point(junction) + " stereotype TRUE roleview_list (list RoleViews";
                        for (int end = 0; end < 2; ++end) {
                            const auto points = roleRoute(r->route, end);
                            const auto target = s.presentations.require(end == 0 ? r->client : r->supplier);
                            const auto roleLabel = QByteArray::number(r->localLabel + end + 1);
                            output += " (object RoleView " + quote("$UNNAMED$" + QString::number(end), encoding) + " @" + roleLabel
                                + " Parent_View @" + QByteArray::number(r->localLabel) + " location (0, 0) stereotype TRUE quidu "
                                + quote(relation.properties.value(QString("roles.%1.quid").arg(end)), encoding)
                                + " client @" + QByteArray::number(r->localLabel) + " supplier @" + QByteArray::number(target.localLabel)
                                + " vertices (list Points";
                            for (const auto &p : points) output += ' ' + point(p);
                            output += ") line_style 0 origin_attachment " + point(points.front()) + " terminal_attachment " + point(points.back()) + ')';
                        }
                        output += ')';
                    } else {
                        const auto client = s.presentations.require(r->client), supplier = s.presentations.require(r->supplier);
                        output += " client @" + QByteArray::number(client.localLabel) + " supplier @" + QByteArray::number(supplier.localLabel) + " vertices (list Points";
                        for (auto p : r->route) output += ' ' + point(p);
                        output += ')';
                        if (!r->route.isEmpty()) output += " line_style 0 origin_attachment " + point(r->route.front()) + " terminal_attachment " + point(r->route.back());
                    }
                    output += newProperties(*r, encoding);
                } else fail(ErrorCode::DanglingReference, "Serialization object is absent");
                QMap<QString, QByteArray> lists;
                for (const auto &child : children.value(id)) {
                    const auto key = listKey(objects.value(child).kind, o.kind); if (key.isEmpty()) fail(ErrorCode::UnsupportedProfile, "No native owner-list serialization rule");
                    lists[key] += ' ' + render(child);
                }
                if (o.kind == "Class_Category") { if (!lists.contains("logical_models")) lists.insert("logical_models", {}); if (!lists.contains("logical_presentations")) lists.insert("logical_presentations", {}); }
                if (o.kind == "ClassDiagram" && !lists.contains("items")) lists.insert("items", {});
                for (auto i = lists.cbegin(); i != lists.cend(); ++i) output += ' ' + i.key().toLatin1() + " (list " + listType(i.key()).toLatin1() + i.value() + ')';
                output += ')';
            }
            rendering.remove(id); return output;
        };
        // Group owner-list insertions. Parent insertions render their whole new subtree.
        QVector<QString> insertionOrder;
        for (const auto &id : relocated) insertionOrder.append(id);
        auto depth = [&](QString id) { int count = 0; while (objects.contains(id) && !objects.value(id).owner.isEmpty()) { ++count; id = objects.value(id).owner; } return count; };
        std::sort(insertionOrder.begin(), insertionOrder.end(), [&](const QString &a, const QString &b) {
            const auto aDepth = depth(a), bDepth = depth(b);
            if (aDepth != bDepth) return aDepth > bDepth;
            const auto &aOwner = objects.constFind(a)->owner, &bOwner = objects.constFind(b)->owner;
            if (aOwner != bOwner) return aOwner < bOwner;
            if (viewOrder.contains(a) && viewOrder.contains(b)) return viewOrder.value(a) < viewOrder.value(b);
            return a < b;
        });
        for (const auto &id : insertionOrder) {
            const auto o = objects.value(id);
            if (o.owner.isEmpty()) fail(ErrorCode::UnsafeRewrite, "Cannot insert an additional native root");
            const auto parent = objects.value(o.owner);
            if (parent.location.node < 0 || parent.copied) continue;
            const auto key = listKey(o.kind, parent.kind); if (key.isEmpty()) fail(ErrorCode::UnsupportedProfile, "No native insertion list for kind: " + o.kind);
            const auto &d = documents[parent.location.unit].document; const auto &node = d.nodes()[parent.location.node];
            const auto &separator = lineEndings[parent.location.unit];
            int list = -1;
            for (const auto &p : node.properties) if (d.raw(p.key) == key.toLatin1()) {
                if (list >= 0 || p.value.child < 0 || d.nodes()[p.value.child].parts.isEmpty() || d.raw(d.nodes()[p.value.child].parts[0].span) != "list") fail(ErrorCode::UnsafeRewrite, "Owner list is ambiguous or not native");
                list = p.value.child;
            }
            QByteArray payload = render(id);
            if (list >= 0) {
                const auto at = viewInsertion.value(id, d.nodes()[list].span.offset + d.nodes()[list].span.length - 1);
                additions[parent.location.unit].append({{at, 0}, separator + payload});
            }
            else {
                // Multiple absent lists are coalesced below; one native property per key.
                const qsizetype at = node.span.offset + node.span.length - 1;
                const QByteArray prefix = ' ' + key.toLatin1() + " (list " + listType(key).toLatin1();
                bool merged = false;
                for (auto &p : additions[parent.location.unit]) if (p.first.offset == at && p.second.startsWith(prefix)) { p.second.chop(1); p.second += separator + payload + ')'; merged = true; break; }
                if (!merged) additions[parent.location.unit].append({{at, 0}, prefix + separator + payload + ')'});
            }
        }
        QMap<UnitId, QByteArray> result; QVector<UnitDocument> validation;
        for (int u = 0; u < documents.size(); ++u) {
            const auto &d = documents[u].document; auto patches = scalar[u]; patches += removals[u]; patches += additions[u];
            // Edits beneath a deleted/moved native object are carried by its insertion instead.
            QVector<Patch> filtered;
            for (const auto &p : patches) {
                bool covered = false;
                for (const auto &remove : removals[u]) if (p.first.offset >= remove.first.offset && p.first.offset + p.first.length <= remove.first.offset + remove.first.length
                    && !(p.first.offset == remove.first.offset && p.first.length == remove.first.length && p.second.isEmpty())) { covered = true; break; }
                if (!covered) filtered.append(p);
            }
            QByteArray bytes = filtered.isEmpty() ? d.bytes() : slice(d, {0, d.bytes().size()}, std::move(filtered));
            if (!documents[u].writable && bytes != d.bytes()) fail(ErrorCode::AccessDenied, "Serialization modifies a read-only unit", documents[u].path);
            auto parsed = PetalDocument::parse(bytes, d.encoding()); if (auto e = std::get_if<WorkspaceError>(&parsed)) throw *e;
            validation.append({documents[u].id, documents[u].path, std::get<PetalDocument>(std::move(parsed)), documents[u].writable});
            result.insert(documents[u].id, std::move(bytes));
        }
        auto projected = Model::fromDocuments(std::move(validation)); if (auto e = std::get_if<WorkspaceError>(&projected)) throw *e;
        return result;
    } catch (const WorkspaceError &e) { return e; }
}

Outcome<QMap<UnitId, QByteArray>> Model::serialize() const { return impl_->serialize(impl_->state(), impl_->copies); }
} // namespace rose
