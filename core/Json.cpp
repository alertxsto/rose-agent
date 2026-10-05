#include "Json.h"
#include <QJsonDocument>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace rose::json {
namespace {
constexpr double safeInteger = 9007199254740991.0;
WorkspaceError invalid(const QString &message) { return {ErrorCode::InvalidCommand, message, {}}; }
QJsonObject typed(const char *type) { return {{"type", type}}; }
QJsonObject text() { return typed("string"); }
QJsonObject idSchema() { return {{"type", "string"}, {"minLength", 1}, {"pattern", "^[^\\s\\x00-\\x1f\\x7f]+$(?![\\s\\S])"}}; }
QJsonObject optionalIdSchema() { return {{"type", "string"}, {"pattern", "^[^\\s\\x00-\\x1f\\x7f]*$(?![\\s\\S])"}}; }
// Unit identities are canonical filesystem paths, not whitespace-free Rose IDs.
QJsonObject unitIdSchema() { return {{"type", "string"}, {"pattern", "^[^\\x00]*$(?![\\s\\S])"}}; }
QString decimalRange(const QString &maximum) {
    QStringList choices{"[1-9][0-9]{0," + QString::number(maximum.size() - 2) + "}"};
    QString prefix;
    for (qsizetype i = 0; i < maximum.size(); ++i) {
        int lower = i == 0 ? 1 : 0;
        int upper = maximum.at(i).digitValue() - 1;
        if (upper >= lower) {
            QString digits = upper == lower ? QString::number(lower) : "[" + QString::number(lower) + "-" + QString::number(upper) + "]";
            choices.append(prefix + digits + "[0-9]{" + QString::number(maximum.size() - i - 1) + "}");
        }
        prefix += maximum.at(i);
    }
    choices.append(maximum);
    return "(?:" + choices.join('|') + ")";
}
QJsonObject decimal(bool signedValue = false) {
    static const QString unsignedPattern = "^(?:0|" + decimalRange("18446744073709551615") + ")$(?![\\s\\S])";
    static const QString signedPattern = "^(?:0|" + decimalRange("9223372036854775807") + "|-" + decimalRange("9223372036854775808") + ")$(?![\\s\\S])";
    return {{"type", "string"}, {"pattern", signedValue ? signedPattern : unsignedPattern}};
}
QJsonObject closed(QJsonObject properties, QJsonArray required) {
    return {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}};
}
QJsonObject array(QJsonObject items, int minimum = 0) { return {{"type", "array"}, {"items", items}, {"minItems", minimum}}; }
QJsonObject constant(const QString &value) { return {{"type", "string"}, {"const", value}}; }
QJsonObject alternatives(QJsonArray values) { return {{"oneOf", values}}; }
QJsonObject integerNumber(double maximum = safeInteger) { return {{"type", "integer"}, {"minimum", 0}, {"maximum", maximum}}; }
QJsonObject signedInteger() { return alternatives({QJsonObject{{"type", "integer"}, {"minimum", -safeInteger}, {"maximum", safeInteger}}, closed({{"type", constant("integer")}, {"value", decimal(true)}}, {"type", "value"})}); }
QJsonObject sizeSchema() {
    const auto maximum = std::numeric_limits<qsizetype>::max();
    QJsonObject fullWidth{{"type", "string"}, {"pattern", "^(?:0|" + decimalRange(QString::number(maximum)) + ")$(?![\\s\\S])"}};
    return alternatives({integerNumber(std::min(safeInteger, double(maximum))), closed({{"type", constant("integer")}, {"value", fullWidth}}, {"type", "value"})});
}
QJsonObject valueSchema() {
    return alternatives({text(), typed("boolean"), typed("number"), closed({{"type", constant("integer")}, {"value", decimal(true)}}, {"type", "value"}), closed({{"type", constant("reference")}, {"value", decimal()}}, {"type", "value"})});
}
QJsonObject objectIdSchema() {
    return closed({{"kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"element", "relation", "diagram", "presentation"}}}}, {"value", idSchema()}}, {"kind", "value"});
}
QJsonObject pointSchema() { return closed({{"x", typed("number")}, {"y", typed("number")}}, {"x", "y"}); }
QJsonObject geometrySchema() {
    return closed({{"x", typed("number")}, {"y", typed("number")}, {"width", QJsonObject{{"type", "number"}, {"exclusiveMinimum", 0}}}, {"height", QJsonObject{{"type", "number"}, {"exclusiveMinimum", 0}}}}, {"x", "y", "width", "height"});
}
const QMap<QString, QJsonObject> &commandSchemas() {
    static const QMap<QString, QJsonObject> schemas = [] {
        QMap<QString, QJsonObject> result;
        auto add = [&](const QString &name, QJsonObject fields) {
            fields.insert("type", constant(name)); QJsonArray required;
            for (auto it = fields.begin(); it != fields.end(); ++it) required.append(it.key());
            result.insert(name, closed(fields, required));
        };
        add("createElement", {{"clientId", idSchema()}, {"kind", idSchema()}, {"name", text()}, {"owner", optionalIdSchema()}, {"unit", unitIdSchema()}});
        add("renameElement", {{"id", idSchema()}, {"name", text()}});
        add("setProperty", {{"id", objectIdSchema()}, {"key", idSchema()}, {"value", valueSchema()}});
        add("setOwner", {{"id", idSchema()}, {"owner", optionalIdSchema()}});
        add("copyElements", {{"ids", array(idSchema(), 1)}, {"owner", optionalIdSchema()}, {"clientIds", QJsonObject{{"type", "object"}, {"additionalProperties", idSchema()}, {"propertyNames", idSchema()}}}});
        add("createRelation", {{"clientId", idSchema()}, {"kind", idSchema()}, {"name", text()}, {"owner", optionalIdSchema()}, {"endpoints", array(idSchema(), 2)}, {"properties", QJsonObject{{"type", "object"}, {"additionalProperties", valueSchema()}, {"propertyNames", idSchema()}}}});
        add("reconnectRelation", {{"id", idSchema()}, {"endpoints", array(idSchema(), 2)}});
        add("createDiagram", {{"clientId", idSchema()}, {"kind", idSchema()}, {"name", text()}, {"owner", optionalIdSchema()}, {"unit", unitIdSchema()}});
        add("addPresentation", {{"clientId", idSchema()}, {"diagram", idSchema()}, {"subject", objectIdSchema()}, {"geometry", geometrySchema()}});
        add("setGeometry", {{"id", idSchema()}, {"geometry", geometrySchema()}});
        add("setRoute", {{"id", idSchema()}, {"points", array(pointSchema(), 2)}});
        add("setMessageOrder", {{"id", idSchema()}, {"ordinal", signedInteger()}});
        add("removePresentation", {{"id", idSchema()}});
        add("deleteElement", {{"id", idSchema()}, {"acknowledgedDependents", array(idSchema())}});
        return result;
    }();
    return schemas;
}
QJsonObject commandSchema() { QJsonArray choices; for (const auto &schema : commandSchemas()) choices.append(schema); return alternatives(choices); }
const QMap<QString, QJsonObject> &querySchemas() {
    static const QMap<QString, QJsonObject> schemas = [] {
        QMap<QString, QJsonObject> result;
        auto add = [&](QString name, QJsonObject fields, QJsonArray required) {
            fields.insert("kind", constant(name)); fields.insert("offset", sizeSchema()); fields.insert("limit", sizeSchema()); required.append("kind");
            result.insert(name, closed(fields, required));
        };
        add("modelTree", {}, {}); add("elementsById", {{"elements", array(idSchema(), 1)}}, {"elements"});
        add("search", {{"search", text()}}, {"search"}); add("diagramById", {{"diagram", idSchema()}}, {"diagram"});
        add("neighborhood", {{"focus", idSchema()}, {"depth", integerNumber(std::numeric_limits<int>::max())}}, {"focus"});
        add("diagnostics", {}, {}); return result;
    }(); return schemas;
}
QJsonObject querySchema() {
    QJsonArray choices;
    for (const auto &schema : querySchemas()) choices.append(schema);
    // Chat-completions function parameters require an explicit object root.
    return {{"type", "object"}, {"oneOf", choices}};
}
QJsonObject approvalSchema() { return closed({{"id", idSchema()}, {"baseRevision", decimal()}, {"digest", QJsonObject{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$(?![\\s\\S])"}}}}, {"id", "baseRevision", "digest"}); }
QString validate(const QJsonValue &value, const QJsonObject &schema, const QString &path = "$ ") {
    if (schema.contains("oneOf")) {
        int matches = 0; for (const auto &choice : schema.value("oneOf").toArray()) if (validate(value, choice.toObject(), path).isEmpty()) ++matches;
        return matches == 1 ? QString{} : path + "does not match exactly one supported shape";
    }
    const auto type = schema.value("type").toString();
    bool matches = type == "object" ? value.isObject() : type == "array" ? value.isArray() : type == "string" ? value.isString() : type == "boolean" ? value.isBool() : value.isDouble() && std::isfinite(value.toDouble());
    if (type == "integer") matches = matches && std::trunc(value.toDouble()) == value.toDouble();
    if (!matches) return path + "must be " + type;
    if (schema.contains("const") && value != schema.value("const")) return path + "has an unsupported discriminant";
    if (schema.contains("enum") && !schema.value("enum").toArray().contains(value)) return path + "has an unsupported value";
    if (value.isString()) {
        if (value.toString().size() < schema.value("minLength").toInt()) return path + "must not be empty";
        if (schema.contains("pattern") && !QRegularExpression(schema.value("pattern").toString()).match(value.toString()).hasMatch()) return path + "has an invalid format";
    }
    if (value.isDouble()) {
        double number = value.toDouble();
        if ((schema.contains("minimum") && number < schema.value("minimum").toDouble()) || (schema.contains("maximum") && number > schema.value("maximum").toDouble()) || (schema.contains("exclusiveMinimum") && number <= schema.value("exclusiveMinimum").toDouble())) return path + "is out of range";
    }
    if (value.isArray()) {
        const auto values = value.toArray(); if (values.size() < schema.value("minItems").toInt()) return path + "has too few entries";
        for (qsizetype i = 0; i < values.size(); ++i) { auto failure = validate(values[i], schema.value("items").toObject(), path + QString::number(i) + "."); if (!failure.isEmpty()) return failure; }
    }
    if (value.isObject()) {
        const auto object = value.toObject(), properties = schema.value("properties").toObject();
        for (const auto &key : schema.value("required").toArray()) if (!object.contains(key.toString())) return path + key.toString() + " is required";
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (schema.contains("propertyNames")) { auto failure = validate(it.key(), schema.value("propertyNames").toObject(), path); if (!failure.isEmpty()) return failure; }
            QJsonObject child;
            if (properties.contains(it.key())) child = properties.value(it.key()).toObject();
            else if (schema.value("additionalProperties").isObject()) child = schema.value("additionalProperties").toObject();
            else return path + it.key() + " is unknown";
            auto failure = validate(it.value(), child, path + it.key() + "."); if (!failure.isEmpty()) return failure;
        }
    }
    return {};
}
Outcome<quint64> unsignedDecimal(const QJsonValue &value) {
    const auto failure = validate(value, decimal()); if (!failure.isEmpty()) return invalid(failure);
    bool ok = false; auto number = value.toString().toULongLong(&ok, 10); if (!ok) return invalid("Unsigned decimal exceeds uint64"); return number;
}
Outcome<qint64> integer(const QJsonValue &value) {
    if (value.isDouble()) {
        const auto failure = validate(value, QJsonObject{{"type", "integer"}, {"minimum", -safeInteger}, {"maximum", safeInteger}});
        if (!failure.isEmpty()) return invalid(failure); return qint64(value.toDouble());
    }
    const auto failure = validate(value, closed({{"type", constant("integer")}, {"value", decimal(true)}}, {"type", "value"}));
    if (!failure.isEmpty()) return invalid(failure);
    bool ok = false; auto number = value.toObject().value("value").toString().toLongLong(&ok, 10); if (!ok) return invalid("Signed decimal exceeds int64"); return number;
}
Outcome<EncodedValue> encoded(const QJsonValue &value) {
    if (value.isString()) return EncodedValue{value.toString()};
    if (value.isBool()) return EncodedValue{value.toBool()};
    if (value.isDouble()) {
        double number = value.toDouble();
        if (!std::isfinite(number)) return invalid("Number must be finite");
        if (std::trunc(number) == number && std::abs(number) <= safeInteger) return EncodedValue{qint64(number)};
        return EncodedValue{number};
    }
    const auto object = value.toObject();
    if (object.value("type") == "reference") { auto parsed = unsignedDecimal(object.value("value")); if (auto e = std::get_if<WorkspaceError>(&parsed)) return *e; return EncodedValue{LocalReference{std::get<quint64>(parsed)}}; }
    auto parsed = integer(value); if (auto e = std::get_if<WorkspaceError>(&parsed)) return *e; return EncodedValue{std::get<qint64>(parsed)};
}
QJsonValue encoded(const EncodedValue &value) {
    return std::visit([](const auto &v) -> QJsonValue {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, LocalReference>) return QJsonObject{{"type", "reference"}, {"value", QString::number(v.value)}};
        else if constexpr (std::is_same_v<T, qint64>) { if (v < -9007199254740991LL || v > 9007199254740991LL) return QJsonObject{{"type", "integer"}, {"value", QString::number(v)}}; return QJsonValue(v); }
        else return QJsonValue(v);
    }, value);
}
ObjectId objectId(const QJsonObject &value) {
    auto id = value.value("value").toString(); auto kind = value.value("kind").toString();
    if (kind == "relation") return RelationId{id}; if (kind == "diagram") return DiagramId{id}; if (kind == "presentation") return PresentationId{id}; return ElementId{id};
}
QJsonObject objectId(const ObjectId &value) {
    return std::visit([](const auto &id) -> QJsonObject { using T = std::decay_t<decltype(id)>; const char *kind = "element"; if constexpr (std::is_same_v<T, RelationId>) kind = "relation"; else if constexpr (std::is_same_v<T, DiagramId>) kind = "diagram"; else if constexpr (std::is_same_v<T, PresentationId>) kind = "presentation"; return {{"kind", kind}, {"value", id.value}}; }, value);
}
Geometry geometry(const QJsonObject &value) { return {value.value("x").toDouble(), value.value("y").toDouble(), value.value("width").toDouble(), value.value("height").toDouble()}; }
QJsonObject geometry(const Geometry &value) { return {{"x", value.x}, {"y", value.y}, {"width", value.width}, {"height", value.height}}; }
template<class T> QVector<T> ids(const QJsonArray &values) { QVector<T> result; result.reserve(values.size()); for (const auto &value : values) result.append(T{value.toString()}); return result; }
template<class T> QJsonArray ids(const QVector<T> &values) { QJsonArray result; for (const auto &value : values) result.append(value.value); return result; }
QJsonObject strings(const QMap<QString, QString> &values) { QJsonObject result; for (auto it = values.begin(); it != values.end(); ++it) result.insert(it.key(), it.value()); return result; }
QJsonArray points(const QVector<WorldPoint> &values) { QJsonArray result; for (const auto &p : values) result.append(QJsonObject{{"x", p.x}, {"y", p.y}}); return result; }
QJsonArray diagnostics(const QVector<Diagnostic> &values) {
    QJsonArray result; for (const auto &d : values) result.append(QJsonObject{{"severity", d.severity == Severity::Error ? "error" : d.severity == Severity::Warning ? "warning" : "information"}, {"code", d.code}, {"message", d.message}, {"file", d.file}, {"objectId", d.objectId}, {"span", QJsonObject{{"offset", encoded(EncodedValue{qint64(d.span.offset)})}, {"length", encoded(EncodedValue{qint64(d.span.length)})}}}}); return result;
}
const char *queryName(Query::Kind kind) { switch (kind) { case Query::Kind::ModelTree: return "modelTree"; case Query::Kind::ElementsById: return "elementsById"; case Query::Kind::Search: return "search"; case Query::Kind::DiagramById: return "diagramById"; case Query::Kind::Neighborhood: return "neighborhood"; case Query::Kind::Diagnostics: return "diagnostics"; } return "modelTree"; }
QByteArray canonical(const QJsonValue &value) {
    if (value.isObject()) { QByteArray result = "{"; auto object = value.toObject(); const auto keys = object.keys(); bool first = true; for (const auto &key : keys) { if (!first) result += ','; first = false; result += canonical(QJsonValue(key)) + ':' + canonical(object.value(key)); } return result + '}'; }
    if (value.isArray()) { QByteArray result = "["; bool first = true; for (const auto &entry : value.toArray()) { if (!first) result += ','; first = false; result += canonical(entry); } return result + ']'; }
    QByteArray encoded = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact); return encoded.mid(1, encoded.size() - 2);
}
} // namespace

Outcome<Revision> revision(const QJsonValue &value) { return unsignedDecimal(value); }
Outcome<Query> query(const QJsonObject &object) {
    const auto name = object.value("kind").toString();
    if (!querySchemas().contains(name)) return invalid("Unknown query kind");
    const auto failure = validate(object, querySchemas().value(name)); if (!failure.isEmpty()) return invalid(failure);
    Query result;
    if (name == "elementsById") result.kind = Query::Kind::ElementsById;
    else if (name == "search") result.kind = Query::Kind::Search;
    else if (name == "diagramById") result.kind = Query::Kind::DiagramById;
    else if (name == "neighborhood") result.kind = Query::Kind::Neighborhood;
    else if (name == "diagnostics") result.kind = Query::Kind::Diagnostics;
    result.elements = ids<ElementId>(object.value("elements").toArray()); result.search = object.value("search").toString(); result.diagram = DiagramId{object.value("diagram").toString()}; result.focus = ElementId{object.value("focus").toString()};
    if (object.contains("depth")) result.depth = object.value("depth").toInt();
    for (const auto &key : {QStringLiteral("offset"), QStringLiteral("limit")}) if (object.contains(key)) {
        auto parsed = integer(object.value(key)); if (auto error = std::get_if<WorkspaceError>(&parsed)) return *error;
        const auto number = std::get<qint64>(parsed);
        if (key == "offset") result.offset = qsizetype(number); else result.limit = qsizetype(number);
    }
    return result;
}
Outcome<QVector<Command>> commands(const QJsonArray &values) {
    if (values.isEmpty()) return invalid("Command batch must not be empty");
    QVector<Command> result; result.reserve(values.size());
    for (const auto &value : values) {
        if (!value.isObject()) return invalid("Command must be an object");
        const auto object = value.toObject(); const auto type = object.value("type").toString();
        if (!commandSchemas().contains(type)) return invalid("Unknown command type");
        auto failure = validate(value, commandSchemas().value(type)); if (!failure.isEmpty()) return invalid(failure);
        const auto id = object.value("id").toString(), clientId = object.value("clientId").toString(), kind = object.value("kind").toString(), name = object.value("name").toString();
        ElementId owner{object.value("owner").toString()}; UnitId unit{object.value("unit").toString()};
        if (type == "createElement") result.append({CreateElement{clientId, kind, name, owner, unit}});
        else if (type == "renameElement") result.append({RenameElement{ElementId{id}, name}});
        else if (type == "setProperty") { auto parsed = encoded(object.value("value")); if (auto e = std::get_if<WorkspaceError>(&parsed)) return *e; result.append({SetProperty{objectId(object.value("id").toObject()), object.value("key").toString(), std::get<EncodedValue>(parsed)}}); }
        else if (type == "setOwner") result.append({SetOwner{ElementId{id}, owner}});
        else if (type == "copyElements") {
            QMap<ElementId, QString> mapping; const auto entries = object.value("clientIds").toObject(); for (auto it = entries.begin(); it != entries.end(); ++it) mapping.insert(ElementId{it.key()}, it.value().toString());
            result.append({CopyElements{ids<ElementId>(object.value("ids").toArray()), owner, mapping}});
        } else if (type == "createRelation") {
            QMap<QString, EncodedValue> properties; const auto entries = object.value("properties").toObject();
            for (auto it = entries.begin(); it != entries.end(); ++it) { auto parsed = encoded(it.value()); if (auto e = std::get_if<WorkspaceError>(&parsed)) return *e; properties.insert(it.key(), std::get<EncodedValue>(parsed)); }
            result.append({CreateRelation{clientId, kind, name, owner, ids<ElementId>(object.value("endpoints").toArray()), properties}});
        } else if (type == "reconnectRelation") result.append({ReconnectRelation{RelationId{id}, ids<ElementId>(object.value("endpoints").toArray())}});
        else if (type == "createDiagram") result.append({CreateDiagram{clientId, kind, name, owner, unit}});
        else if (type == "addPresentation") result.append({AddPresentation{clientId, DiagramId{object.value("diagram").toString()}, objectId(object.value("subject").toObject()), geometry(object.value("geometry").toObject())}});
        else if (type == "setGeometry") result.append({SetGeometry{PresentationId{id}, geometry(object.value("geometry").toObject())}});
        else if (type == "setRoute") { QVector<WorldPoint> route; for (const auto &entry : object.value("points").toArray()) { const auto p = entry.toObject(); route.append({p.value("x").toDouble(), p.value("y").toDouble()}); } result.append({SetRoute{PresentationId{id}, route}}); }
        else if (type == "setMessageOrder") { auto parsed = integer(object.value("ordinal")); if (auto e = std::get_if<WorkspaceError>(&parsed)) return *e; result.append({SetMessageOrder{ElementId{id}, std::get<qint64>(parsed)}}); }
        else if (type == "removePresentation") result.append({RemovePresentation{PresentationId{id}}});
        else { QStringList dependents; for (const auto &entry : object.value("acknowledgedDependents").toArray()) dependents.append(entry.toString()); result.append({DeleteElement{ElementId{id}, dependents}}); }
    }
    return result;
}
Outcome<Approval> approval(const QJsonObject &object) {
    auto failure = validate(object, approvalSchema()); if (!failure.isEmpty()) return invalid(failure);
    auto base = revision(object.value("baseRevision")); if (auto e = std::get_if<WorkspaceError>(&base)) return *e;
    return Approval{ProposalId{object.value("id").toString()}, std::get<Revision>(base), QByteArray::fromHex(object.value("digest").toString().toLatin1())};
}
QJsonObject query(const Query &value) {
    QJsonObject result{{"kind", queryName(value.kind)}, {"offset", encoded(EncodedValue{qint64(value.offset)})}, {"limit", encoded(EncodedValue{qint64(value.limit)})}};
    switch (value.kind) { case Query::Kind::ElementsById: result.insert("elements", ids(value.elements)); break; case Query::Kind::Search: result.insert("search", value.search); break; case Query::Kind::DiagramById: result.insert("diagram", value.diagram.value); break; case Query::Kind::Neighborhood: result.insert("focus", value.focus.value); result.insert("depth", value.depth); break; default: break; } return result;
}
QJsonArray commands(const QVector<Command> &values) {
    QJsonArray result;
    for (const auto &command : values) result.append(std::visit([](const auto &v) -> QJsonObject {
        using T = std::decay_t<decltype(v)>; QJsonObject o;
        if constexpr (std::is_same_v<T, CreateElement> || std::is_same_v<T, CreateDiagram>) { o = {{"type", std::is_same_v<T, CreateElement> ? "createElement" : "createDiagram"}, {"clientId", v.clientId}, {"kind", v.kind}, {"name", v.name}, {"owner", v.owner.value}, {"unit", v.unit.value}}; }
        else if constexpr (std::is_same_v<T, RenameElement>) o = {{"type", "renameElement"}, {"id", v.id.value}, {"name", v.name}};
        else if constexpr (std::is_same_v<T, SetProperty>) o = {{"type", "setProperty"}, {"id", objectId(v.id)}, {"key", v.key}, {"value", encoded(v.value)}};
        else if constexpr (std::is_same_v<T, SetOwner>) o = {{"type", "setOwner"}, {"id", v.id.value}, {"owner", v.owner.value}};
        else if constexpr (std::is_same_v<T, CopyElements>) { QJsonObject mapping; for (auto it = v.clientIds.begin(); it != v.clientIds.end(); ++it) mapping.insert(it.key().value, it.value()); o = {{"type", "copyElements"}, {"ids", ids(v.ids)}, {"owner", v.owner.value}, {"clientIds", mapping}}; }
        else if constexpr (std::is_same_v<T, CreateRelation>) { QJsonObject properties; for (auto it = v.properties.begin(); it != v.properties.end(); ++it) properties.insert(it.key(), encoded(it.value())); o = {{"type", "createRelation"}, {"clientId", v.clientId}, {"kind", v.kind}, {"name", v.name}, {"owner", v.owner.value}, {"endpoints", ids(v.endpoints)}, {"properties", properties}}; }
        else if constexpr (std::is_same_v<T, ReconnectRelation>) o = {{"type", "reconnectRelation"}, {"id", v.id.value}, {"endpoints", ids(v.endpoints)}};
        else if constexpr (std::is_same_v<T, AddPresentation>) o = {{"type", "addPresentation"}, {"clientId", v.clientId}, {"diagram", v.diagram.value}, {"subject", objectId(v.subject)}, {"geometry", geometry(v.geometry)}};
        else if constexpr (std::is_same_v<T, SetGeometry>) o = {{"type", "setGeometry"}, {"id", v.id.value}, {"geometry", geometry(v.geometry)}};
        else if constexpr (std::is_same_v<T, SetRoute>) o = {{"type", "setRoute"}, {"id", v.id.value}, {"points", points(v.points)}};
        else if constexpr (std::is_same_v<T, SetMessageOrder>) o = {{"type", "setMessageOrder"}, {"id", v.id.value}, {"ordinal", encoded(EncodedValue{v.ordinal})}};
        else if constexpr (std::is_same_v<T, RemovePresentation>) o = {{"type", "removePresentation"}, {"id", v.id.value}};
        else if constexpr (std::is_same_v<T, DeleteElement>) o = {{"type", "deleteElement"}, {"id", v.id.value}, {"acknowledgedDependents", QJsonArray::fromStringList(v.acknowledgedDependents)}};
        return o;
    }, command.payload)); return result;
}
QJsonObject projection(const Projection &p) {
    QJsonArray elements, relations, diagrams, presentations, units;
    for (const auto &v : p.elements) elements.append(QJsonObject{{"id", v.id.value}, {"kind", v.kind}, {"name", v.name}, {"owner", v.owner.value}, {"unit", v.unit.value}, {"properties", strings(v.properties)}, {"propertyTypes", strings(v.propertyTypes)}, {"readOnly", v.readOnly}});
    for (const auto &v : p.relations) relations.append(QJsonObject{{"id", v.id.value}, {"kind", v.kind}, {"name", v.name}, {"owner", v.owner.value}, {"unit", v.unit.value}, {"endpoints", ids(v.endpoints)}, {"properties", strings(v.properties)}, {"propertyTypes", strings(v.propertyTypes)}, {"readOnly", v.readOnly}});
    for (const auto &v : p.diagrams) diagrams.append(QJsonObject{{"id", v.id.value}, {"kind", v.kind}, {"name", v.name}, {"owner", v.owner.value}, {"unit", v.unit.value}, {"presentations", ids(v.presentations)}, {"readOnly", v.readOnly}});
    for (const auto &v : p.presentations) presentations.append(QJsonObject{{"id", v.id.value}, {"diagram", v.diagram.value}, {"element", v.element.value}, {"relation", v.relation.value}, {"parent", v.parent.value}, {"client", v.client.value}, {"supplier", v.supplier.value}, {"kind", v.kind}, {"label", v.label}, {"localLabel", QString::number(v.localLabel)}, {"geometry", geometry(v.geometry)}, {"route", points(v.route)}, {"unit", v.unit.value}, {"properties", strings(v.properties)}, {"propertyTypes", strings(v.propertyTypes)}, {"readOnly", v.readOnly}});
    for (const auto &v : p.units) units.append(QJsonObject{{"id", v.id.value}, {"path", v.path}, {"writable", v.writable}, {"resolved", v.resolved}});
    return {{"type", "projection"}, {"schemaVersion", 1}, {"revision", QString::number(p.revision)}, {"dirty", p.dirty}, {"canUndo", p.canUndo}, {"canRedo", p.canRedo}, {"path", p.path}, {"elements", elements}, {"relations", relations}, {"diagrams", diagrams}, {"presentations", presentations}, {"diagnostics", diagnostics(p.diagnostics)}, {"units", units}, {"total", encoded(EncodedValue{qint64(p.total)})}};
}
QJsonObject proposal(const Proposal &p) {
    QJsonArray diff; for (const auto &v : p.diff) diff.append(QJsonObject{{"kind", v.kind}, {"objectId", v.objectId}, {"field", v.field}, {"before", v.before}, {"after", v.after}});
    return {{"type", "proposal"}, {"id", p.id.value}, {"baseRevision", QString::number(p.baseRevision)}, {"digest", QString::fromLatin1(p.digest.toHex())}, {"diff", diff}, {"diagnostics", diagnostics(p.diagnostics)}, {"newIds", strings(p.newIds)}};
}
QJsonObject approval(const Approval &p) { return {{"id", p.id.value}, {"baseRevision", QString::number(p.baseRevision)}, {"digest", QString::fromLatin1(p.digest.toHex())}}; }
QJsonObject applied(const Applied &p) { return {{"type", "applied"}, {"revision", QString::number(p.revision)}, {"transactionId", p.transactionId}, {"newIds", strings(p.newIds)}}; }
QJsonObject saved(const SaveReceipt &p) { QJsonObject checksums; for (auto it = p.checksums.begin(); it != p.checksums.end(); ++it) checksums.insert(it.key().value, QString::fromLatin1(it.value().toHex())); return {{"type", "saved"}, {"revision", QString::number(p.revision)}, {"checksums", checksums}, {"path", p.path}}; }
QJsonObject error(const WorkspaceError &p) {
    const char *code = "invalidCommand";
    switch (p.code) { case ErrorCode::InvalidSyntax: code = "invalidSyntax"; break; case ErrorCode::UnsupportedProfile: code = "unsupportedProfile"; break; case ErrorCode::UnsupportedEncoding: code = "unsupportedEncoding"; break; case ErrorCode::MissingUnit: code = "missingUnit"; break; case ErrorCode::AccessDenied: code = "accessDenied"; break; case ErrorCode::InvalidCommand: break; case ErrorCode::DanglingReference: code = "danglingReference"; break; case ErrorCode::StaleRevision: code = "staleRevision"; break; case ErrorCode::StaleApproval: code = "staleApproval"; break; case ErrorCode::DiskConflict: code = "diskConflict"; break; case ErrorCode::UnsafeRewrite: code = "unsafeRewrite"; break; case ErrorCode::StorageFailure: code = "storageFailure"; break; }
    return {{"type", "error"}, {"code", code}, {"message", p.message}, {"file", p.file}, {"offset", encoded(EncodedValue{qint64(p.offset)})}, {"line", encoded(EncodedValue{qint64(p.line)})}, {"column", encoded(EncodedValue{qint64(p.column)})}};
}
QJsonObject reply(const WorkspaceReply &value) {
    return std::visit([](const auto &v) -> QJsonObject {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Projection>) return projection(v);
        else if constexpr (std::is_same_v<T, Proposal>) return proposal(v);
        else if constexpr (std::is_same_v<T, Applied>) return applied(v);
        else if constexpr (std::is_same_v<T, SaveReceipt>) return saved(v);
        else if constexpr (std::is_same_v<T, WorkspaceError>) return error(v);
        else if constexpr (std::is_same_v<T, Rejected>) return {{"type", "rejected"}, {"id", v.id.value}};
        else if constexpr (std::is_same_v<T, AccessGranted>) return {{"type", "accessGranted"}, {"directory", v.directory}};
        else return {{"type", "closed"}};
    }, value);
}
QByteArray canonicalCommands(const QVector<Command> &values) { return canonical(QJsonObject{{"schemaVersion", 1}, {"commands", commands(values)}}); }
QJsonArray tools() {
    return {QJsonObject{{"type", "function"}, {"function", QJsonObject{{"name", "inspect"}, {"description", "Inspect the open model without modifying it; choose a query kind and explicit pagination."}, {"parameters", querySchema()}}}}, QJsonObject{{"type", "function"}, {"function", QJsonObject{{"name", "submit_proposal"}, {"description", "Stage a complete command batch at an exact revision for human review. This never applies or saves changes."}, {"parameters", closed({{"baseRevision", decimal()}, {"commands", array(commandSchema(), 1)}}, {"baseRevision", "commands"})}}}}};
}
} // namespace rose::json
