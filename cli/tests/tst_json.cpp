#include <QtTest>
#include "core/Json.h"
#include <limits>

using namespace rose;
class JsonTests final : public QObject {
    Q_OBJECT
private slots:
    void decimalBoundaries() {
        auto max = json::revision(QJsonValue("18446744073709551615"));
        QVERIFY(std::holds_alternative<Revision>(max));
        QCOMPARE(std::get<Revision>(max), std::numeric_limits<quint64>::max());
        for (const QJsonValue &value : {QJsonValue(1), QJsonValue("01"), QJsonValue("-1"), QJsonValue("18446744073709551616"), QJsonValue("1\n"), QJsonValue("+1"), QJsonValue(" 1")})
            QVERIFY(std::holds_alternative<WorkspaceError>(json::revision(value)));
    }
    void rejectsAmbiguousAndMalformedCommands() {
        const QList<QJsonObject> invalid = {
            {{"type", "renameElement"}, {"id", "class"}, {"name", "New"}, {"extra", true}},
            {{"type", "renameElement"}, {"id", 5}, {"name", "New"}},
            {{"type", "setGeometry"}, {"id", "view"}, {"geometry", QJsonObject{{"x", 0}, {"y", 0}, {"width", -1}, {"height", 2}}}},
            {{"type", "setRoute"}, {"id", "view"}, {"points", QJsonArray{QJsonObject{{"x", 0}, {"y", 0}}}}},
            {{"type", "setProperty"}, {"id", QJsonObject{{"kind", "unit"}, {"value", "x"}}}, {"key", "n"}, {"value", 1}},
            {{"type", "setProperty"}, {"id", QJsonObject{{"kind", "element"}, {"value", "x"}}}, {"key", "n"}, {"value", QJsonObject{{"type", "integer"}, {"value", "9223372036854775808"}}}}
        };
        for (const auto &command : invalid)
            QVERIFY(std::holds_alternative<WorkspaceError>(json::commands(QJsonArray{command})));
        QVERIFY(std::holds_alternative<WorkspaceError>(json::query(QJsonObject{{"kind", "modelTree"}, {"depth", "2"}})));
        QVERIFY(std::holds_alternative<WorkspaceError>(json::approval(QJsonObject{{"id", "p"}, {"baseRevision", "0"}, {"digest", "BAD"}})));
        QVERIFY(std::holds_alternative<WorkspaceError>(json::commands(QJsonArray{})));
        QVERIFY(std::holds_alternative<WorkspaceError>(json::commands(QJsonArray{QJsonObject{{"type", "renameElement"}, {"id", "class\n"}, {"name", "New"}}})));
        QVERIFY(std::holds_alternative<WorkspaceError>(json::query(QJsonObject{{"kind", "neighborhood"}, {"focus", "class"}, {"depth", 1.5}})));
        QVERIFY(std::holds_alternative<WorkspaceError>(json::query(QJsonObject{{"kind", "modelTree"}, {"offset", -1}})));
        QVERIFY(std::holds_alternative<WorkspaceError>(json::approval(QJsonObject{{"id", "p"}, {"baseRevision", "0"}, {"digest", QString(64, 'a') + '\n'}})));
    }
    void losslessValuesAndCanonicalDigestInput() {
        QVector<Command> original{
            {SetProperty{ElementId{"class"}, "integer", std::numeric_limits<qint64>::min()}},
            {SetProperty{PresentationId{"view"}, "reference", LocalReference{std::numeric_limits<quint64>::max()}}},
            {RenameElement{ElementId{"class"}, "New"}}
        };
        const auto encoded = json::commands(original);
        auto decoded = json::commands(encoded);
        QVERIFY(std::holds_alternative<QVector<Command>>(decoded));
        const auto &commands = std::get<QVector<Command>>(decoded);
        QCOMPARE(std::get<qint64>(std::get<SetProperty>(commands[0].payload).value), std::numeric_limits<qint64>::min());
        QCOMPARE(std::get<LocalReference>(std::get<SetProperty>(commands[1].payload).value).value, std::numeric_limits<quint64>::max());
        QCOMPARE(json::canonicalCommands(commands), json::canonicalCommands(original));
        const auto envelope = QJsonDocument::fromJson(json::canonicalCommands(original)).object();
        QCOMPARE(envelope.value("schemaVersion").toInt(), 1);
        QCOMPARE(envelope.value("commands").toArray(), encoded);
    }
    void completeBatchPreservesTypedPayloads() {
        const Geometry geometry{10, -20, 120, 80};
        QVector<Command> batch{
            {CreateElement{"newClass", "Class", "Line", ElementId{"root"}, UnitId{}}},
            {RenameElement{ElementId{"newClass"}, "Renamed"}},
            {SetProperty{RelationId{"relation"}, "documentation", QString("Details")}},
            {SetOwner{ElementId{"newClass"}, ElementId{"owner"}}},
            {CopyElements{{ElementId{"class"}}, ElementId{"owner"}, {{ElementId{"class"}, "copy"}}}},
            {CreateRelation{"newRelation", "Association", "Link", ElementId{"owner"}, {ElementId{"class"}, ElementId{"newClass"}}, {{"enabled", true}, {"ratio", 1.5}}}},
            {ReconnectRelation{RelationId{"relation"}, {ElementId{"class"}, ElementId{"newClass"}}}},
            {CreateDiagram{"newDiagram", "ClassDiagram", "View", ElementId{"owner"}, UnitId{"unit"}}},
            {AddPresentation{"newView", DiagramId{"newDiagram"}, ElementId{"newClass"}, geometry}},
            {SetGeometry{PresentationId{"view"}, geometry}},
            {SetRoute{PresentationId{"edge"}, {{0, 1}, {2, 3}}}},
            {SetMessageOrder{ElementId{"message"}, std::numeric_limits<qint64>::max()}},
            {RemovePresentation{PresentationId{"oldView"}}},
            {DeleteElement{ElementId{"oldClass"}, {"dependent"}}}
        };
        auto decoded = json::commands(json::commands(batch));
        QVERIFY(std::holds_alternative<QVector<Command>>(decoded));
        const auto &values = std::get<QVector<Command>>(decoded);
        QCOMPARE(std::get<CreateElement>(values[0].payload).clientId, QString("newClass"));
        QCOMPARE(std::get<RenameElement>(values[1].payload).name, QString("Renamed"));
        QCOMPARE(std::get<RelationId>(std::get<SetProperty>(values[2].payload).id).value, QString("relation"));
        QCOMPARE(std::get<SetOwner>(values[3].payload).owner.value, QString("owner"));
        QCOMPARE(std::get<CopyElements>(values[4].payload).clientIds.value(ElementId{"class"}), QString("copy"));
        QCOMPARE(std::get<bool>(std::get<CreateRelation>(values[5].payload).properties.value("enabled")), true);
        QCOMPARE(std::get<ReconnectRelation>(values[6].payload).endpoints[1].value, QString("newClass"));
        QCOMPARE(std::get<CreateDiagram>(values[7].payload).unit.value, QString("unit"));
        QCOMPARE(std::get<ElementId>(std::get<AddPresentation>(values[8].payload).subject).value, QString("newClass"));
        QCOMPARE(std::get<SetGeometry>(values[9].payload).geometry, geometry);
        QCOMPARE(std::get<SetRoute>(values[10].payload).points[1], (WorldPoint{2, 3}));
        QCOMPARE(std::get<SetMessageOrder>(values[11].payload).ordinal, std::numeric_limits<qint64>::max());
        QCOMPARE(std::get<RemovePresentation>(values[12].payload).id.value, QString("oldView"));
        QCOMPARE(std::get<DeleteElement>(values[13].payload).acknowledgedDependents, QStringList{"dependent"});
    }
    void paginationSurvivesFullWidthSizes() {
        Query source; source.offset = 7; source.limit = std::numeric_limits<qsizetype>::max();
        auto parsed = json::query(json::query(source));
        QVERIFY(std::holds_alternative<Query>(parsed));
        QCOMPARE(std::get<Query>(parsed).offset, qsizetype(7));
        QCOMPARE(std::get<Query>(parsed).limit, std::numeric_limits<qsizetype>::max());
        QVERIFY(std::holds_alternative<WorkspaceError>(json::query(QJsonObject{{"kind", "modelTree"}, {"limit", QJsonObject{{"type", "integer"}, {"value", "18446744073709551615"}}}})));
    }
};
QTEST_GUILESS_MAIN(JsonTests)
#include "tst_json.moc"
