#include <QtTest>
#include "core/Command.h"
#include "core/Json.h"
#include <algorithm>

using namespace rose;
using UnitBytes = QMap<UnitId, QByteArray>;

static QByteArray fixture() {
    return "(object Petal version 50 charSet 0)\n"
        "(object Design \"Logical View\" quid \"400000000000\"\n"
        " root_category (object Class_Category \"Logical View\" quid \"400000000001\"\n"
        "  logical_models (list unit_reference_list\n"
        "   (object Class \"Order\" quid \"400000000002\" vendorExtension (\"UnknownKind\" 201)\n"
        "    class_attributes (list class_attribute_list (object ClassAttribute \"total\" quid \"400000000003\" type \"double\"))))\n"
        "  logical_presentations (list unit_reference_list\n"
        "   (object ClassDiagram \"One\" quid \"400000000004\" title \"One\" items (list diagram_item_list\n"
        "    (object ClassView \"Class\" \"Logical View::Order\" @1 location (100, 100) quidu \"400000000002\" width 200 height 100\n"
        "     label (object ItemLabel Parent_View @1 label \"Order\"))))\n"
        "   (object ClassDiagram \"Two\" quid \"400000000005\" title \"Two\" items (list diagram_item_list\n"
        "    (object ClassView \"Class\" \"Logical View::Order\" @2 location (500, 200) quidu \"400000000002\" width 200 height 100\n"
        "     label (object ItemLabel Parent_View @2 label \"Order\")))))))\n";
}
static Outcome<std::unique_ptr<Model>> model(QByteArray bytes = fixture(), QString unit = "main") {
    auto parsed = PetalDocument::parse(std::move(bytes));
    if (auto *failure = std::get_if<WorkspaceError>(&parsed)) return *failure;
    QVector<UnitDocument> units;
    units.append({UnitId{unit}, unit + ".mdl", std::get<PetalDocument>(std::move(parsed)), true});
    return Model::fromDocuments(std::move(units));
}

class ProjectionTests final : public QObject {
    Q_OBJECT
private slots:
    void renameUpdatesAllAppearancesWithoutMovingThem();
    void invalidBatchChangesNothing();
    void copiedOwnershipPreservesOriginalsAndOpaqueData();
    void geometryAndHideAffectOnlyOneAppearance();
    void localLabelsAreScopedToSourceUnit();
    void renameRefreshesAssociationRoleSuppliers();
    void associationRoleIdentitiesPersistAndRemainReadOnly();
    void rootRequiresDeclaredSupportedProfile();
    void whitespaceUnitPresentationIdsRemainCommandable();
    void rootCategoryRenameUpdatesReciprocalTypedReferenceOnce();
    void copiedSubtreesRenderCurrentOwnership();
    void independentCopiesDoNotShareSourcePatches();
    void deletionProtectsNestedRoleReferences();
    void copiedTypedReferencesRefreshFromSourceSpelling();
    void reconnectRefreshesNativeViews();
    void controlledDocumentRootCannotBeDeleted();
    void typedScalarEditsRespectNativeBinding_data();
    void typedScalarEditsRespectNativeBinding();
    void associationViewsPreserveOpaqueChildrenAndReconnect();
    void mixedProfilesAreRejectedWithoutRewritingUnits();
    void unsupportedRelationshipsStayOpaqueAndProtectReferences();
    void diagramCompartmentsAndNeighborhoodDependentsPaginate();
    void authoredRoutesFollowFinalEndpointGeometry();
    void nativeViewDeclarationsPrecedeConsumers_data();
    void nativeViewDeclarationsPrecedeConsumers();
};

void ProjectionTests::diagramCompartmentsAndNeighborhoodDependentsPaginate() {
    auto opened = model(); QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
    auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
    const ElementId focus{"400000000002"};
    auto created = workspace.stage({
        Command{CreateElement{"method", "Operation", "submit", focus, {}}},
        Command{SetProperty{ElementId{"method"}, "result", QString("bool")}},
        Command{CreateElement{"parameter", "Parameter", "urgent", ElementId{"method"}, {}}},
        Command{SetProperty{ElementId{"parameter"}, "type", QString("bool")}},
        Command{CreateRelation{"self", "Association", "related", ElementId{"400000000001"}, {focus, focus}, {}}},
        Command{AddPresentation{"link", DiagramId{"400000000004"}, RelationId{"self"}, {}}},
    });
    QVERIFY(std::holds_alternative<ModelDelta>(created));
    const auto createDelta = std::get<ModelDelta>(created);
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(createDelta)));
    const auto beforeDelete = std::get<UnitBytes>(workspace.serialize());
    Query diagram; diagram.kind = Query::Kind::DiagramById; diagram.diagram = DiagramId{"400000000004"};
    const auto compartments = std::get<Projection>(workspace.inspect(diagram, 0));
    QCOMPARE(compartments.elements.size(), 4);
    QCOMPARE(compartments.relations.size(), 1);
    QCOMPARE(compartments.presentations.size(), 2);
    const auto parameter = std::find_if(compartments.elements.cbegin(), compartments.elements.cend(), [](const auto &e) { return e.kind == "Parameter"; });
    QVERIFY(parameter != compartments.elements.cend());
    QCOMPARE(parameter->properties.value("type"), QString("bool"));
    QCOMPARE(parameter->owner.value, createDelta.newIds.value("method"));
    Query neighborhood; neighborhood.kind = Query::Kind::Neighborhood; neighborhood.focus = focus; neighborhood.depth = 100;
    const auto dependents = std::get<Projection>(workspace.inspect(neighborhood, 0));
    QCOMPARE(dependents.presentations.size(), 3); // Both appearances plus the incident connector.
    QCOMPARE(dependents.diagrams.size(), 2);
    auto rowIds = [](const Projection &p) {
        QStringList ids;
        for (const auto &e : p.elements) ids.append(e.id.value);
        for (const auto &r : p.relations) ids.append(r.id.value);
        for (const auto &d : p.diagrams) ids.append(d.id.value);
        for (const auto &v : p.presentations) ids.append(v.id.value);
        return ids;
    };
    for (const auto &query : {diagram, neighborhood}) {
        const auto all = std::get<Projection>(workspace.inspect(query, 0));
        const auto expected = rowIds(all);
        QCOMPARE(all.total, expected.size());
        QStringList paged;
        for (qsizetype offset = 0; offset <= all.total; ++offset) {
            auto pageQuery = query; pageQuery.offset = offset; pageQuery.limit = 1;
            const auto page = std::get<Projection>(workspace.inspect(pageQuery, 0));
            QCOMPARE(page.total, all.total);
            const auto ids = rowIds(page);
            QCOMPARE(ids.size(), offset == all.total ? 0 : 1);
            paged.append(ids);
        }
        QCOMPARE(paged, expected);
    }
    QStringList acknowledgement{QString("400000000003"), createDelta.newIds.value("method"), createDelta.newIds.value("parameter"), createDelta.newIds.value("self")};
    for (const auto &view : dependents.presentations) acknowledgement.append(view.id.value);
    QVERIFY(std::holds_alternative<WorkspaceError>(workspace.stage({Command{DeleteElement{focus, {}}}})));
    auto removed = workspace.stage({Command{DeleteElement{focus, acknowledgement}}});
    QVERIFY(std::holds_alternative<ModelDelta>(removed));
    const auto removeDelta = std::get<ModelDelta>(removed);
    QCOMPARE(removeDelta.presentations.size(), 3);
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(removeDelta)));
    const auto hidden = std::get<Projection>(workspace.inspect(diagram, 1));
    QVERIFY(hidden.elements.isEmpty()); QVERIFY(hidden.relations.isEmpty()); QVERIFY(hidden.presentations.isEmpty());
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(removeDelta, false)));
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()), beforeDelete);
}

void ProjectionTests::authoredRoutesFollowFinalEndpointGeometry() {
    auto opened = model(); QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
    auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
    const ElementId a{"400000000002"};
    const DiagramId diagram{"400000000004"};
    auto staged = workspace.stage({
        Command{CreateElement{"b", "Class", "Entity", ElementId{"400000000001"}, {}}},
        Command{AddPresentation{"bv", diagram, ElementId{"b"}, {600, 100, 200, 100}}},
        Command{SetGeometry{PresentationId{"main#400000000004@1"}, {200, 200, 200, 100}}},
        Command{CreateRelation{"inheritance", "Inheritance_Relationship", {}, a, {a, ElementId{"b"}}, {}}},
        Command{CreateRelation{"association", "Association", "owns", ElementId{"400000000001"}, {a, ElementId{"b"}}, {}}},
        Command{AddPresentation{"inheritv", diagram, RelationId{"inheritance"}, {}}},
        Command{AddPresentation{"assocv", diagram, RelationId{"association"}, {}}},
        Command{SetGeometry{PresentationId{"main#400000000004@1"}, {300, 300, 200, 100}}},
        Command{SetGeometry{PresentationId{"bv"}, {700, 200, 200, 100}}},
    });
    QVERIFY(std::holds_alternative<ModelDelta>(staged));
    const auto delta = std::get<ModelDelta>(staged);
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta)));
    auto emitted = workspace.serialize(); QVERIFY(std::holds_alternative<UnitBytes>(emitted));
    auto reopened = model(std::get<UnitBytes>(emitted).value(UnitId{"main"}));
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(reopened));
    Query query; query.kind = Query::Kind::DiagramById; query.diagram = diagram;
    const auto graph = std::get<Projection>(std::get<std::unique_ptr<Model>>(reopened)->inspect(query, 0));
    bool inheritanceSeen = false, associationSeen = false;
    for (const auto &view : graph.presentations) {
        if (view.kind == "InheritView") {
            inheritanceSeen = true; QCOMPARE(view.route, (QVector<WorldPoint>{{400, 275}, {600, 225}}));
        }
        if (view.kind == "AssociationViewNew") {
            associationSeen = true; QCOMPARE(view.route, (QVector<WorldPoint>{{400, 275}, {400, 150}, {600, 225}}));
        }
    }
    QVERIFY(inheritanceSeen); QVERIFY(associationSeen);
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta, false)));
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), fixture());
}

void ProjectionTests::nativeViewDeclarationsPrecedeConsumers_data() {
    QTest::addColumn<bool>("newDiagram");
    QTest::addColumn<int>("version");
    QTest::newRow("insert-into-source-diagram-44") << false << 44;
    QTest::newRow("render-authored-diagram-44") << true << 44;
    QTest::newRow("insert-into-source-diagram-50") << false << 50;
    QTest::newRow("render-authored-diagram-50") << true << 50;
}

void ProjectionTests::nativeViewDeclarationsPrecedeConsumers() {
    QFETCH(bool, newDiagram);
    QFETCH(int, version);
    auto source = fixture(); source.replace("version 50", "version " + QByteArray::number(version));
    auto opened = model(source); QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
    auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
    const ElementId owner{"400000000001"}, order{"400000000002"};
    DiagramId diagram{"400000000004"};
    QVector<Command> commands;
    if (newDiagram) {
        commands.append(Command{CreateDiagram{"native", "ClassDiagram", "Native", owner, {}}});
        commands.append(Command{AddPresentation{"orderView", DiagramId{"native"}, order, {100, 100, 200, 100}}});
        diagram = DiagramId{"native"};
    }
    // Cross the @9/@10 lexical-ID boundary as well as hash insertion order.
    for (int i = 0; i < 12; ++i) {
        const auto id = QString("class%1").arg(i), view = QString("view%1").arg(i);
        commands.append(Command{CreateElement{id, "Class", QString("Entity%1").arg(i), owner, {}}});
        commands.append(Command{AddPresentation{view, diagram, ElementId{id}, {300.0 + i * 250, 200, 200, 100}}});
    }
    commands.append(Command{CreateRelation{"link", "Association", "owns", owner, {ElementId{"class0"}, ElementId{"class11"}}, {}}});
    commands.append(Command{AddPresentation{"linkView", diagram, RelationId{"link"}, {}}});
    commands.append(Command{CreateRelation{"inherit", "Inheritance_Relationship", {}, order, {order, ElementId{"class11"}}, {}}});
    commands.append(Command{AddPresentation{"inheritView", diagram, RelationId{"inherit"}, {}}});
    auto staged = workspace.stage(commands); QVERIFY(std::holds_alternative<ModelDelta>(staged));
    const auto delta = std::get<ModelDelta>(staged);
    if (newDiagram) diagram.value = delta.newIds.value("native");
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta)));
    // Model's two-pass projection accepts forward references. This deliberately
    // consumes native declarations in source order, including nested RoleViews
    // and ItemLabels, as Rose requires, rather than asserting a textual layout.
    auto consumerFailure = [&](const QByteArray &bytes) {
        auto parsed = PetalDocument::parse(bytes);
        if (const auto *e = std::get_if<WorkspaceError>(&parsed)) return e->message;
        const auto &native = std::get<PetalDocument>(parsed);
        const auto object = native.objectIndex("ClassDiagram", diagram.value.toLatin1());
        if (!object) return QString("Missing class diagram");
        const auto span = native.nodes()[*object].span;
        QVector<int> nodes;
        for (int n = 0; n < native.nodes().size(); ++n) {
            const auto child = native.nodes()[n].span;
            if (child.offset >= span.offset && child.offset + child.length <= span.offset + span.length) nodes.append(n);
        }
        std::sort(nodes.begin(), nodes.end(), [&](int a, int b) { return native.nodes()[a].span.offset < native.nodes()[b].span.offset; });
        QSet<QByteArray> declared;
        for (int n : nodes) {
            const auto &node = native.nodes()[n];
            for (qsizetype p = 2; p < node.headerParts; ++p) if (node.parts[p].kind == AtomKind::Reference) {
                const auto label = native.raw(node.parts[p].span).toByteArray();
                if (declared.contains(label)) return QString("Duplicate declaration: ") + QString::fromLatin1(label);
                declared.insert(label);
            }
            for (const auto &property : node.properties) if (property.value.kind == AtomKind::Reference && property.value.child < 0) {
                const auto label = native.raw(property.value.span).toByteArray();
                if (!declared.contains(label)) return QString("Unresolved native reference: ") + QString::fromLatin1(label);
            }
        }
        return QString{};
    };
    auto emitted = workspace.serialize(); QVERIFY(std::holds_alternative<UnitBytes>(emitted));
    const auto baseline = std::get<UnitBytes>(emitted).value(UnitId{"main"});
    QCOMPARE(consumerFailure(baseline), QString{});
    auto reopened = model(baseline); QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(reopened));
    Query query; query.kind = Query::Kind::DiagramById; query.diagram = diagram;
    const auto graph = std::get<Projection>(std::get<std::unique_ptr<Model>>(reopened)->inspect(query, 0));
    QCOMPARE(graph.presentations.size(), 15); QCOMPARE(graph.relations.size(), 2);
    // Existing native routes now consume a declaration authored in a later edit.
    auto &savedWorkspace = *std::get<std::unique_ptr<Model>>(reopened);
    auto reconnected = savedWorkspace.stage({
        Command{CreateElement{"late", "Class", "Late", owner, {}}},
        Command{AddPresentation{"lateView", diagram, ElementId{"late"}, {900, 500, 200, 100}}},
        Command{ReconnectRelation{RelationId{delta.newIds.value("link")}, {ElementId{delta.newIds.value("class0")}, ElementId{"late"}}}},
        Command{ReconnectRelation{RelationId{delta.newIds.value("inherit")}, {order, ElementId{"late"}}}},
    });
    QVERIFY(std::holds_alternative<ModelDelta>(reconnected));
    const auto reconnectDelta = std::get<ModelDelta>(reconnected);
    QVERIFY(std::holds_alternative<std::monostate>(savedWorkspace.applyDelta(reconnectDelta)));
    auto changed = savedWorkspace.serialize(); QVERIFY(std::holds_alternative<UnitBytes>(changed));
    const auto changedBytes = std::get<UnitBytes>(changed).value(UnitId{"main"});
    QCOMPARE(consumerFailure(changedBytes), QString{});
    auto lateOpened = model(changedBytes); QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(lateOpened));
    const auto lateGraph = std::get<Projection>(std::get<std::unique_ptr<Model>>(lateOpened)->inspect(query, 1));
    QCOMPARE(lateGraph.presentations.size(), 16); QCOMPARE(lateGraph.relations.size(), 2);
    for (const auto &view : lateGraph.presentations) if (!view.relation.isEmpty())
        QCOMPARE(view.supplier.value, reconnectDelta.newIds.value("lateView"));
    QVERIFY(std::holds_alternative<std::monostate>(savedWorkspace.applyDelta(reconnectDelta, false)));
    QCOMPARE(std::get<UnitBytes>(savedWorkspace.serialize()).value(UnitId{"main"}), baseline);
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta, false)));
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), source);
}

void ProjectionTests::renameUpdatesAllAppearancesWithoutMovingThem() {
    auto opened = model();
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
    auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
    auto change = workspace.stage({Command{RenameElement{ElementId{"400000000002"}, "Payment"}}});
    QVERIFY(std::holds_alternative<ModelDelta>(change));
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(std::get<ModelDelta>(change))));
    auto bytes = workspace.serialize();
    QVERIFY(std::holds_alternative<UnitBytes>(bytes));
    auto parsed = PetalDocument::parse(std::get<UnitBytes>(bytes).value(UnitId{"main"}));
    QVERIFY(std::holds_alternative<PetalDocument>(parsed));
    const auto &after = std::get<PetalDocument>(parsed);
    QCOMPARE(after.objectName("Class", "400000000002")->text(), QStringLiteral("Payment"));
    for (int object : after.objectsOfKind("ClassView")) {
        QCOMPARE(std::get<QString>(after.text(after.nodes()[object].parts[3])), QStringLiteral("Logical View::Payment"));
        const int label = after.nodes()[object].properties.back().value.child;
        QCOMPARE(after.property(label, "label")->rawValue(after), ByteView("\"Payment\""));
    }
    QCOMPARE(after.property(after.objectsOfKind("ClassView")[0], "location")->rawValue(after), ByteView("(100, 100)"));
    QCOMPARE(after.property(after.objectsOfKind("ClassView")[1], "location")->rawValue(after), ByteView("(500, 200)"));
    QCOMPARE(after.objectProperty("Class", "400000000002", "vendorExtension")->rawValue(after), ByteView("(\"UnknownKind\" 201)"));
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(std::get<ModelDelta>(change), false)));
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), fixture());
}

void ProjectionTests::invalidBatchChangesNothing() {
    auto opened = model();
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
    auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
    auto result = workspace.stage({Command{RenameElement{ElementId{"400000000002"}, "Payment"}},
        Command{SetOwner{ElementId{"400000000001"}, ElementId{"400000000002"}}}});
    QVERIFY(std::holds_alternative<WorkspaceError>(result));
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), fixture());
}

void ProjectionTests::copiedOwnershipPreservesOriginalsAndOpaqueData() {
    auto opened = model();
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
    auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
    CopyElements copy{{ElementId{"400000000002"}}, ElementId{"400000000001"},
        {{ElementId{"400000000002"}, "copy-order"}, {ElementId{"400000000003"}, "copy-total"}}};
    auto staged = workspace.stage({Command{copy}});
    QVERIFY(std::holds_alternative<ModelDelta>(staged));
    const auto &delta = std::get<ModelDelta>(staged);
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta)));
    auto emitted = workspace.serialize();
    QVERIFY(std::holds_alternative<UnitBytes>(emitted));
    auto parsed = PetalDocument::parse(std::get<UnitBytes>(emitted).value(UnitId{"main"}));
    QVERIFY(std::holds_alternative<PetalDocument>(parsed));
    const auto &after = std::get<PetalDocument>(parsed);
    const auto copiedId = delta.newIds.value("copy-order").toLatin1();
    const auto copiedAttribute = delta.newIds.value("copy-total").toLatin1();
    QCOMPARE(after.objectName("Class", "400000000002")->text(), QStringLiteral("Order"));
    QCOMPARE(after.objectName("Class", copiedId)->text(), QStringLiteral("Order"));
    QCOMPARE(after.objectProperty("Class", copiedId, "vendorExtension")->rawValue(after), ByteView("(\"UnknownKind\" 201)"));
    const int attribute = *after.objectIndex("ClassAttribute", copiedAttribute);
    const int owner = after.nodes()[after.nodes()[attribute].parent].parent;
    QCOMPARE(owner, *after.objectIndex("Class", copiedId));
}

void ProjectionTests::geometryAndHideAffectOnlyOneAppearance() {
    auto opened = model();
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
    auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
    Query query; query.kind = Query::Kind::DiagramById; query.diagram = DiagramId{"400000000004"};
    const auto initial = std::get<Projection>(workspace.inspect(query, 0));
    QCOMPARE(initial.presentations.size(), 1);
    const auto id = initial.presentations.front().id;
    auto staged = workspace.stage({Command{SetGeometry{id, {300, 400, 250, 120}}}});
    QVERIFY(std::holds_alternative<ModelDelta>(staged));
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(std::get<ModelDelta>(staged))));
    query.diagram = DiagramId{"400000000005"};
    const auto other = std::get<Projection>(workspace.inspect(query, 1));
    QCOMPARE(other.presentations.front().geometry.x, 500);
    QCOMPARE(other.presentations.front().geometry.y, 200);
    auto hidden = workspace.stage({Command{RemovePresentation{id}}});
    QVERIFY(std::holds_alternative<ModelDelta>(hidden));
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(std::get<ModelDelta>(hidden))));
    auto parsed = PetalDocument::parse(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}));
    QVERIFY(std::holds_alternative<PetalDocument>(parsed));
    const auto &after = std::get<PetalDocument>(parsed);
    QCOMPARE(after.objectsOfKind("ClassView").size(), 1);
    QVERIFY(after.objectIndex("Class", "400000000002").has_value());
    QCOMPARE(after.property(after.objectsOfKind("ClassView").front(), "location")->rawValue(after), ByteView("(500, 200)"));
}

void ProjectionTests::localLabelsAreScopedToSourceUnit() {
    QByteArray first = fixture();
    QByteArray second = fixture();
    second.replace("40000000000", "50000000000");
    auto a = PetalDocument::parse(first), b = PetalDocument::parse(second);
    QVERIFY(std::holds_alternative<PetalDocument>(a));
    QVERIFY(std::holds_alternative<PetalDocument>(b));
    auto opened = Model::fromDocuments({{UnitId{"a"}, "a.mdl", std::get<PetalDocument>(a), true},
        {UnitId{"b"}, "b.mdl", std::get<PetalDocument>(b), true}});
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
    auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
    Query query; query.kind = Query::Kind::DiagramById; query.diagram = DiagramId{"400000000004"};
    const auto one = std::get<Projection>(workspace.inspect(query, 0));
    query.diagram = DiagramId{"500000000004"};
    const auto two = std::get<Projection>(workspace.inspect(query, 0));
    QCOMPARE(one.presentations.front().localLabel, two.presentations.front().localLabel);
    QVERIFY(one.presentations.front().id != two.presentations.front().id);
    QCOMPARE(one.presentations.front().element, ElementId{"400000000002"});
    QCOMPARE(two.presentations.front().element, ElementId{"500000000002"});
}

void ProjectionTests::renameRefreshesAssociationRoleSuppliers() {
    const QByteArray source =
        "(object Petal version 50 charSet 0)\n"
        "(object Class_Category \"Domain\" quid \"600000000001\" logical_models (list unit_reference_list\n"
        " (object Class \"Order\" quid \"600000000002\")\n"
        " (object Class \"Customer\" quid \"600000000003\")\n"
        " (object Association \"billedTo\" quid \"600000000004\" roles (list role_list\n"
        "  (object Role \"order\" quid \"600000000005\" supplier \"Domain::Order\" quidu \"600000000002\" client_cardinality (value cardinality \"*\") foreignRole (\"opaque\" 7))\n"
        "  (object Role \"customer\" quid \"600000000006\" supplier \"Domain::Customer\" quidu \"600000000003\" client_cardinality (value cardinality \"1\"))))))\n";
    auto opened = model(source);
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
    auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
    auto staged = workspace.stage({Command{RenameElement{ElementId{"600000000001"}, "Billing"}},
        Command{RenameElement{ElementId{"600000000002"}, "Invoice"}}});
    QVERIFY(std::holds_alternative<ModelDelta>(staged));
    const auto delta = std::get<ModelDelta>(staged);
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta)));
    auto bytes = workspace.serialize();
    QVERIFY(std::holds_alternative<UnitBytes>(bytes));
    auto parsed = PetalDocument::parse(std::get<UnitBytes>(bytes).value(UnitId{"main"}));
    QVERIFY(std::holds_alternative<PetalDocument>(parsed));
    const auto &after = std::get<PetalDocument>(parsed);
    QCOMPARE(after.objectProperty("Role", "600000000005", "supplier")->rawValue(after), ByteView("\"Billing::Invoice\""));
    QCOMPARE(after.objectProperty("Role", "600000000006", "supplier")->rawValue(after), ByteView("\"Billing::Customer\""));
    QCOMPARE(after.objectProperty("Role", "600000000005", "quidu")->rawValue(after), ByteView("\"600000000002\""));
    QCOMPARE(after.objectProperty("Role", "600000000005", "client_cardinality")->rawValue(after), ByteView("(value cardinality \"*\")"));
    QCOMPARE(after.objectProperty("Role", "600000000005", "foreignRole")->rawValue(after), ByteView("(\"opaque\" 7)"));
    Query query; query.limit = 100;
    const auto graph = std::get<Projection>(workspace.inspect(query, 1));
    QCOMPARE(graph.relations.front().endpoints, (QVector<ElementId>{ElementId{"600000000002"}, ElementId{"600000000003"}}));
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta, false)));
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), source);
}

void ProjectionTests::associationRoleIdentitiesPersistAndRemainReadOnly() {
    auto opened = model();
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
    auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
    CreateRelation relation{"self", "Association", "related", ElementId{"400000000001"},
        {ElementId{"400000000002"}, ElementId{"400000000002"}}, {}};
    auto staged = workspace.stage({Command{relation}});
    QVERIFY(std::holds_alternative<ModelDelta>(staged));
    const auto delta = std::get<ModelDelta>(staged);
    const auto record = delta.relations.front().after.value();
    const auto first = record.properties.value("roles.0.quid"), second = record.properties.value("roles.1.quid");
    QVERIFY(first != second);
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta)));
    auto emitted = workspace.serialize();
    QVERIFY(std::holds_alternative<UnitBytes>(emitted));
    const auto bytes = std::get<UnitBytes>(emitted).value(UnitId{"main"});
    auto parsed = PetalDocument::parse(bytes);
    QVERIFY(std::holds_alternative<PetalDocument>(parsed));
    const auto &native = std::get<PetalDocument>(parsed);
    QVERIFY(native.objectIndex("Role", first.toLatin1()).has_value());
    QVERIFY(native.objectIndex("Role", second.toLatin1()).has_value());
    QVERIFY(!native.objectProperty("Association", record.id.value.toLatin1(), "roles.0.quid").has_value());
    auto reopened = model(bytes);
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(reopened));
    Query query; query.limit = 100;
    const auto graph = std::get<Projection>(std::get<std::unique_ptr<Model>>(reopened)->inspect(query, 0));
    QCOMPARE(graph.relations.front().properties.value("roles.0.quid"), first);
    QCOMPARE(graph.relations.front().properties.value("roles.1.quid"), second);
    auto edit = workspace.stage({Command{SetProperty{record.id, "roles.0.quid", QString("BAD")}}});
    QVERIFY(std::holds_alternative<WorkspaceError>(edit));
    relation.clientId = "bad"; relation.properties.insert("roles.0.quid", QString("BAD"));
    QVERIFY(std::holds_alternative<WorkspaceError>(workspace.stage({Command{relation}})));
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), bytes);
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta, false)));
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), fixture());
}

void ProjectionTests::rootRequiresDeclaredSupportedProfile() {
    for (const auto &source : {QByteArray("(object Class \"Bare\" quid \"400000000002\")"),
            QByteArray("(object Petal version 47 charSet 0)\n(object Class \"Legacy\" quid \"400000000002\")"),
            QByteArray("(object Petal version 50 charSet 0)")}) {
        auto opened = model(source);
        QVERIFY(std::holds_alternative<WorkspaceError>(opened));
        QCOMPARE(std::get<WorkspaceError>(opened).code, ErrorCode::UnsupportedProfile);
    }
}

void ProjectionTests::whitespaceUnitPresentationIdsRemainCommandable() {
    const QString unit = "Shared Components/model";
    auto opened = model(fixture(), unit);
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
    auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
    Query query; query.kind = Query::Kind::DiagramById; query.diagram = DiagramId{"400000000004"};
    const auto graph = std::get<Projection>(workspace.inspect(query, 0));
    const auto id = graph.presentations.front().id;
    const auto parsedCommands = json::commands(json::commands({Command{SetGeometry{id, {300, 400, 250, 120}}}}));
    QVERIFY(std::holds_alternative<QVector<Command>>(parsedCommands));
    auto staged = workspace.stage(std::get<QVector<Command>>(parsedCommands));
    QVERIFY(std::holds_alternative<ModelDelta>(staged));
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(std::get<ModelDelta>(staged))));
    auto emitted = workspace.serialize();
    QVERIFY(std::holds_alternative<UnitBytes>(emitted));
    auto reopened = model(std::get<UnitBytes>(emitted).value(UnitId{unit}), unit);
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(reopened));
    const auto after = std::get<Projection>(std::get<std::unique_ptr<Model>>(reopened)->inspect(query, 1));
    QCOMPARE(after.presentations.front().id, id);
    QCOMPARE(after.presentations.front().geometry, (Geometry{300, 400, 250, 120}));
    QVERIFY(std::holds_alternative<WorkspaceError>(workspace.stage({Command{CreateElement{
        id.value, "Class", "Ambiguous", ElementId{"400000000001"}, UnitId{unit}}}})));
    QVERIFY(std::holds_alternative<WorkspaceError>(workspace.stage({Command{CreateElement{
        "400000000002", "Class", "Ambiguous", ElementId{"400000000001"}, UnitId{unit}}}})));
}

void ProjectionTests::rootCategoryRenameUpdatesReciprocalTypedReferenceOnce() {
    const QByteArray source =
        "(object Petal version 50 charSet 0)\n"
        "(object Design \"Logical View\" quid \"610000000000\"\n"
        " root_category (object Class_Category \"Logical View\" quid \"610000000001\" subsystem \"Component View\" quidu \"610000000002\")\n"
        " root_subsystem (object SubSystem \"Component View\" quid \"610000000002\" category \"Logical View\" quidu \"610000000001\"))\n";
    auto opened = model(source);
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
    auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
    auto staged = workspace.stage({Command{RenameElement{ElementId{"610000000001"}, "Domain"}}});
    QVERIFY(std::holds_alternative<ModelDelta>(staged));
    const auto delta = std::get<ModelDelta>(staged);
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta)));
    Query query; query.kind = Query::Kind::ElementsById; query.elements = {ElementId{"610000000002"}};
    const auto after = std::get<Projection>(workspace.inspect(query, 1));
    QCOMPARE(after.elements.front().properties.value("category"), QString("Domain"));
    auto emitted = workspace.serialize();
    QVERIFY(std::holds_alternative<UnitBytes>(emitted));
    auto parsed = PetalDocument::parse(std::get<UnitBytes>(emitted).value(UnitId{"main"}));
    QVERIFY(std::holds_alternative<PetalDocument>(parsed));
    const auto &native = std::get<PetalDocument>(parsed);
    QCOMPARE(native.objectProperty("SubSystem", "610000000002", "category")->rawValue(native), ByteView("\"Domain\""));
    QCOMPARE(native.objectProperty("SubSystem", "610000000002", "quidu")->rawValue(native), ByteView("\"610000000001\""));
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta, false)));
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), source);
    auto ambiguousSource = source;
    ambiguousSource.replace("category \"Logical View\"", "category \"unrelated\"");
    auto ambiguous = model(ambiguousSource);
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(ambiguous));
    auto &unsafe = *std::get<std::unique_ptr<Model>>(ambiguous);
    auto denied = unsafe.stage({Command{RenameElement{ElementId{"610000000001"}, "Domain"}}});
    QVERIFY(std::holds_alternative<WorkspaceError>(denied));
    QCOMPARE(std::get<WorkspaceError>(denied).code, ErrorCode::UnsafeRewrite);
    QCOMPARE(std::get<UnitBytes>(unsafe.serialize()).value(UnitId{"main"}), ambiguousSource);
}

void ProjectionTests::copiedSubtreesRenderCurrentOwnership() {
    for (bool removeChild : {false, true}) {
        auto opened = model();
        QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
        auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
        CopyElements copy{{ElementId{"400000000002"}}, ElementId{"400000000001"},
            {{ElementId{"400000000002"}, "copy-order"}, {ElementId{"400000000003"}, "copy-total"}}};
        auto copied = workspace.stage({Command{copy}});
        QVERIFY(std::holds_alternative<ModelDelta>(copied));
        const auto copyDelta = std::get<ModelDelta>(copied);
        QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(copyDelta)));
        auto savedCopy = workspace.serialize();
        QVERIFY(std::holds_alternative<UnitBytes>(savedCopy));
        const auto copiedBytes = std::get<UnitBytes>(savedCopy).value(UnitId{"main"});
        const auto copiedId = ElementId{copyDelta.newIds.value("copy-order")};
        QVector<Command> commands;
        if (removeChild) commands.append(Command{DeleteElement{ElementId{copyDelta.newIds.value("copy-total")}, {}}});
        else commands.append(Command{CreateElement{"extra", "ClassAttribute", "extra", copiedId, {}}});
        auto staged = workspace.stage(commands);
        QVERIFY(std::holds_alternative<ModelDelta>(staged));
        const auto delta = std::get<ModelDelta>(staged);
        QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta)));
        auto emitted = workspace.serialize();
        QVERIFY(std::holds_alternative<UnitBytes>(emitted));
        const auto bytes = std::get<UnitBytes>(emitted).value(UnitId{"main"});
        auto reopened = model(bytes);
        QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(reopened));
        Query query; query.limit = 100;
        const auto graph = std::get<Projection>(std::get<std::unique_ptr<Model>>(reopened)->inspect(query, 0));
        const auto expectedId = ElementId{removeChild ? copyDelta.newIds.value("copy-total") : delta.newIds.value("extra")};
        const auto child = std::find_if(graph.elements.cbegin(), graph.elements.cend(), [&](const auto &r) { return r.id == expectedId; });
        if (removeChild) QVERIFY(child == graph.elements.cend());
        else { QVERIFY(child != graph.elements.cend()); QCOMPARE(child->owner, copiedId); QCOMPARE(child->name, QString("extra")); }
        auto parsed = PetalDocument::parse(bytes);
        QVERIFY(std::holds_alternative<PetalDocument>(parsed));
        const auto &native = std::get<PetalDocument>(parsed);
        QCOMPARE(native.objectProperty("Class", copiedId.value.toLatin1(), "vendorExtension")->rawValue(native), ByteView("(\"UnknownKind\" 201)"));
        const auto baseline = std::get<PetalDocument>(PetalDocument::parse(fixture()));
        QCOMPARE(native.raw(native.nodes()[*native.objectIndex("Class", "400000000002")].span),
            baseline.raw(baseline.nodes()[*baseline.objectIndex("Class", "400000000002")].span));
        QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta, false)));
        QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), copiedBytes);
        QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(copyDelta, false)));
        QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), fixture());
    }
}

void ProjectionTests::independentCopiesDoNotShareSourcePatches() {
    auto opened = model();
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
    auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
    QVector<ModelDelta> deltas;
    for (int index = 0; index < 2; ++index) {
        const auto alias = QString("copy-%1").arg(index), childAlias = QString("total-%1").arg(index);
        CopyElements copy{{ElementId{"400000000002"}}, ElementId{"400000000001"},
            {{ElementId{"400000000002"}, alias}, {ElementId{"400000000003"}, childAlias}}};
        auto staged = workspace.stage({Command{copy}, Command{RenameElement{ElementId{alias}, QString("Copy%1").arg(index)}}});
        QVERIFY(std::holds_alternative<ModelDelta>(staged));
        deltas.append(std::get<ModelDelta>(staged));
        QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(deltas.back())));
        // Serialization does not replace the immutable original source tree.
        auto emitted = workspace.serialize();
        QVERIFY(std::holds_alternative<UnitBytes>(emitted));
        auto reopened = model(std::get<UnitBytes>(emitted).value(UnitId{"main"}));
        QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(reopened));
        Query query; query.limit = 100;
        const auto graph = std::get<Projection>(std::get<std::unique_ptr<Model>>(reopened)->inspect(query, 0));
        for (int copyIndex = 0; copyIndex <= index; ++copyIndex) {
            const auto root = ElementId{deltas[copyIndex].newIds.value(QString("copy-%1").arg(copyIndex))};
            const auto child = ElementId{deltas[copyIndex].newIds.value(QString("total-%1").arg(copyIndex))};
            auto r = std::find_if(graph.elements.cbegin(), graph.elements.cend(), [&](const auto &e) { return e.id == root; });
            QVERIFY(r != graph.elements.cend()); QCOMPARE(r->name, QString("Copy%1").arg(copyIndex));
            r = std::find_if(graph.elements.cbegin(), graph.elements.cend(), [&](const auto &e) { return e.id == child; });
            QVERIFY(r != graph.elements.cend()); QCOMPARE(r->owner, root);
        }
    }
    for (auto i = deltas.crbegin(); i != deltas.crend(); ++i)
        QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(*i, false)));
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), fixture());
    auto first = CopyElements{{ElementId{"400000000002"}}, ElementId{"400000000001"},
        {{ElementId{"400000000002"}, "first"}, {ElementId{"400000000003"}, "first-total"}}};
    auto second = first; second.clientIds = {{ElementId{"400000000002"}, "second"}, {ElementId{"400000000003"}, "second-total"}};
    auto together = workspace.stage({Command{first}, Command{second},
        Command{RenameElement{ElementId{"first"}, "First"}}, Command{RenameElement{ElementId{"second"}, "Second"}}});
    QVERIFY(std::holds_alternative<ModelDelta>(together));
    const auto delta = std::get<ModelDelta>(together);
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta)));
    auto emitted = workspace.serialize();
    QVERIFY(std::holds_alternative<UnitBytes>(emitted));
    auto reopened = model(std::get<UnitBytes>(emitted).value(UnitId{"main"}));
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(reopened));
    Query query; query.kind = Query::Kind::ElementsById;
    query.elements = {ElementId{delta.newIds.value("first-total")}, ElementId{delta.newIds.value("second-total")}};
    const auto graph = std::get<Projection>(std::get<std::unique_ptr<Model>>(reopened)->inspect(query, 0));
    QCOMPARE(graph.elements[0].owner, ElementId{delta.newIds.value("first")});
    QCOMPARE(graph.elements[1].owner, ElementId{delta.newIds.value("second")});
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta, false)));
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), fixture());
}

void ProjectionTests::deletionProtectsNestedRoleReferences() {
    const QByteArray source =
        "(object Petal version 50 charSet 0)\n"
        "(object Class_Category \"Domain\" quid \"600000000001\" logical_models (list unit_reference_list\n"
        " (object Class \"Order\" quid \"600000000002\")\n"
        " (object Class \"Customer\" quid \"600000000003\" foreignRoleRef \"600000000005\")\n"
        " (object Association \"billedTo\" quid \"600000000004\" roles (list role_list\n"
        "  (object Role \"order\" quid \"600000000005\" supplier \"Domain::Order\" quidu \"600000000002\")\n"
        "  (object Role \"customer\" quid \"600000000006\" supplier \"Domain::Customer\" quidu \"600000000003\")))))\n";
    auto opened = model(source);
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
    auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
    auto denied = workspace.stage({Command{DeleteElement{ElementId{"600000000002"}, {"600000000004"}}}});
    QVERIFY(std::holds_alternative<WorkspaceError>(denied));
    QCOMPARE(std::get<WorkspaceError>(denied).code, ErrorCode::UnsafeRewrite);
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), source);
    // The same guard covers opaque scalar references introduced after opening.
    auto cleanSource = source; cleanSource.replace(" foreignRoleRef \"600000000005\"", "");
    auto clean = model(cleanSource);
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(clean));
    auto &edited = *std::get<std::unique_ptr<Model>>(clean);
    auto introduced = edited.stage({Command{SetProperty{ElementId{"600000000003"}, "documentation", QString("Role 600000000006")}}});
    QVERIFY(std::holds_alternative<ModelDelta>(introduced));
    QVERIFY(std::holds_alternative<std::monostate>(edited.applyDelta(std::get<ModelDelta>(introduced))));
    denied = edited.stage({Command{DeleteElement{ElementId{"600000000002"}, {"600000000004"}}}});
    QVERIFY(std::holds_alternative<WorkspaceError>(denied));
    QCOMPARE(std::get<WorkspaceError>(denied).code, ErrorCode::UnsafeRewrite);
    QVERIFY(std::holds_alternative<std::monostate>(edited.applyDelta(std::get<ModelDelta>(introduced), false)));
    QCOMPARE(std::get<UnitBytes>(edited.serialize()).value(UnitId{"main"}), cleanSource);
}

void ProjectionTests::copiedTypedReferencesRefreshFromSourceSpelling() {
    for (bool qualifiedType : {false, true}) {
        auto source = fixture();
        source.replace("type \"double\"", qualifiedType
            ? "type \"Logical View::Order\" quidu \"400000000002\""
            : "type \"Order\" quidu \"400000000002\"");
        auto opened = model(source);
        QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
        auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
        CopyElements copy{{ElementId{"400000000002"}}, ElementId{"destination"},
            {{ElementId{"400000000002"}, "copy-order"}, {ElementId{"400000000003"}, "copy-total"}}};
        auto staged = workspace.stage({
            Command{CreateElement{"destination", "Class_Category", "Archive", ElementId{"400000000001"}, {}}},
            Command{copy}, Command{RenameElement{ElementId{"copy-order"}, "Invoice"}}});
        QVERIFY(std::holds_alternative<ModelDelta>(staged));
        const auto delta = std::get<ModelDelta>(staged);
        QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta)));
        auto emitted = workspace.serialize();
        QVERIFY(std::holds_alternative<UnitBytes>(emitted));
        auto reopened = model(std::get<UnitBytes>(emitted).value(UnitId{"main"}));
        QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(reopened));
        Query query; query.kind = Query::Kind::ElementsById; query.elements = {ElementId{delta.newIds.value("copy-total")}, ElementId{"400000000003"}};
        const auto graph = std::get<Projection>(std::get<std::unique_ptr<Model>>(reopened)->inspect(query, 0));
        QCOMPARE(graph.elements[0].properties.value("quidu"), delta.newIds.value("copy-order"));
        QCOMPARE(graph.elements[0].properties.value("type"), qualifiedType ? QString("Logical View::Archive::Invoice") : QString("Invoice"));
        QCOMPARE(graph.elements[1].properties.value("type"), qualifiedType ? QString("Logical View::Order") : QString("Order"));
        QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta, false)));
        QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), source);
        copy.owner = ElementId{"400000000001"};
        auto renamedFirst = workspace.stage({Command{RenameElement{ElementId{"400000000002"}, "Invoice"}}, Command{copy}});
        QVERIFY(std::holds_alternative<ModelDelta>(renamedFirst));
        const auto renamedDelta = std::get<ModelDelta>(renamedFirst);
        QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(renamedDelta)));
        emitted = workspace.serialize();
        QVERIFY(std::holds_alternative<UnitBytes>(emitted));
        reopened = model(std::get<UnitBytes>(emitted).value(UnitId{"main"}));
        QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(reopened));
        query.elements = {ElementId{renamedDelta.newIds.value("copy-total")}};
        const auto copiedAfterRename = std::get<Projection>(std::get<std::unique_ptr<Model>>(reopened)->inspect(query, 0));
        QCOMPARE(copiedAfterRename.elements.front().properties.value("type"), qualifiedType ? QString("Logical View::Invoice") : QString("Invoice"));
        QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(renamedDelta, false)));
        QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), source);
        auto ambiguousSource = source;
        ambiguousSource.replace(qualifiedType ? "type \"Logical View::Order\"" : "type \"Order\"", "type \"unrelated\"");
        auto ambiguous = model(ambiguousSource);
        QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(ambiguous));
        copy.owner = ElementId{"400000000001"};
        auto denied = std::get<std::unique_ptr<Model>>(ambiguous)->stage({Command{copy}, Command{RenameElement{ElementId{"copy-order"}, "Invoice"}}});
        QVERIFY(std::holds_alternative<WorkspaceError>(denied));
        QCOMPARE(std::get<WorkspaceError>(denied).code, ErrorCode::UnsafeRewrite);
    }
}

void ProjectionTests::reconnectRefreshesNativeViews() {
    const QByteArray source =
        "(object Petal version 50 charSet 0)\n"
        "(object Class_Category \"Domain\" quid \"640000000001\" logical_models (list unit_reference_list\n"
        " (object Class \"Order\" quid \"640000000002\" superclasses (list inheritance_relationship_list"
        "  (object Inheritance_Relationship quid \"640000000005\" supplier \"Domain::B\" quidu \"640000000003\")))\n"
        " (object Class \"B\" quid \"640000000003\") (object Class \"C\" quid \"640000000004\"))\n"
        " logical_presentations (list unit_reference_list (object ClassDiagram \"Types\" quid \"640000000006\" items (list diagram_item_list\n"
        " (object ClassView \"Class\" \"Domain::Order\" @1 quidu \"640000000002\" location (100, 100) width 200 height 100)\n"
        " (object ClassView \"Class\" \"Domain::B\" @2 quidu \"640000000003\" location (400, 100) width 200 height 100)\n"
        " (object ClassView \"Class\" \"Domain::C\" @3 quidu \"640000000004\" location (700, 100) width 200 height 100)\n"
        " (object InheritView \"\" @4 quidu \"640000000005\" client @1 supplier @2 vertices (list Points (200, 100) (250, 150) (300, 100))"
        " origin_attachment (200, 100) terminal_attachment (300, 100) vendorRoute (\"opaque\" 9))))))\n";
    const Command reconnect{ReconnectRelation{RelationId{"640000000005"}, {ElementId{"640000000002"}, ElementId{"640000000004"}}}};
    auto opened = model(source);
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
    auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
    const PresentationId clientId{"main#640000000006@1"};
    auto moved = workspace.stage({Command{SetGeometry{clientId, {200, 200, 400, 200}}}});
    QVERIFY(std::holds_alternative<ModelDelta>(moved));
    const auto moveDelta = std::get<ModelDelta>(moved);
    const auto changedRoute = std::find_if(moveDelta.presentations.cbegin(), moveDelta.presentations.cend(), [](const auto &c) { return c.after && c.after->kind == "InheritView"; });
    QVERIFY(changedRoute != moveDelta.presentations.cend());
    QCOMPARE(changedRoute->after->route, (QVector<WorldPoint>{{400, 200}, {250, 150}, {300, 100}}));
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(moveDelta)));
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(moveDelta, false)));
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), source);
    auto staged = workspace.stage({reconnect});
    QVERIFY(std::holds_alternative<ModelDelta>(staged));
    const auto delta = std::get<ModelDelta>(staged);
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta)));
    auto emitted = workspace.serialize();
    QVERIFY(std::holds_alternative<UnitBytes>(emitted));
    const auto bytes = std::get<UnitBytes>(emitted).value(UnitId{"main"});
    auto reopened = model(bytes);
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(reopened));
    Query query; query.kind = Query::Kind::DiagramById; query.diagram = DiagramId{"640000000006"};
    const auto graph = std::get<Projection>(std::get<std::unique_ptr<Model>>(reopened)->inspect(query, 0));
    const auto view = std::find_if(graph.presentations.cbegin(), graph.presentations.cend(), [](const auto &p) { return p.kind == "InheritView"; });
    QVERIFY(view != graph.presentations.cend());
    const auto supplier = std::find_if(graph.presentations.cbegin(), graph.presentations.cend(), [&](const auto &p) { return p.id == view->supplier; });
    QVERIFY(supplier != graph.presentations.cend()); QCOMPARE(supplier->element, ElementId{"640000000004"});
    QCOMPARE(view->route, (QVector<WorldPoint>{{200, 100}, {250, 150}, {600, 100}}));
    auto parsed = PetalDocument::parse(bytes);
    QVERIFY(std::holds_alternative<PetalDocument>(parsed));
    const auto &native = std::get<PetalDocument>(parsed);
    QCOMPARE(native.property(native.objectsOfKind("InheritView").front(), "vendorRoute")->rawValue(native), ByteView("(\"opaque\" 9)"));
    QCOMPARE(native.property(native.objectsOfKind("InheritView").front(), "terminal_attachment")->rawValue(native), ByteView("(600, 100)"));
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta, false)));
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), source);
    const QVector<WorldPoint> explicitRoute{{205, 105}, {470, 170}, {680, 120}};
    auto explicitEdit = workspace.stage({Command{SetRoute{view->id, explicitRoute}}, reconnect,
        Command{SetGeometry{clientId, {200, 200, 400, 200}}}});
    QVERIFY(std::holds_alternative<ModelDelta>(explicitEdit));
    const auto &explicitDelta = std::get<ModelDelta>(explicitEdit);
    const auto explicitView = std::find_if(explicitDelta.presentations.cbegin(), explicitDelta.presentations.cend(), [](const auto &c) { return c.after && c.after->kind == "InheritView"; });
    QVERIFY(explicitView != explicitDelta.presentations.cend()); QCOMPARE(explicitView->after->route, explicitRoute);
    auto attachedSource = source;
    attachedSource.replace("vertices (list Points (200, 100) (250, 150) (300, 100))", "");
    auto attached = model(attachedSource); QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(attached));
    auto attachedEdit = std::get<std::unique_ptr<Model>>(attached)->stage({reconnect});
    QVERIFY(std::holds_alternative<ModelDelta>(attachedEdit));
    const auto &attachedDelta = std::get<ModelDelta>(attachedEdit);
    QVERIFY(std::holds_alternative<std::monostate>(std::get<std::unique_ptr<Model>>(attached)->applyDelta(attachedDelta)));
    const auto attachedBytes = std::get<UnitBytes>(std::get<std::unique_ptr<Model>>(attached)->serialize()).value(UnitId{"main"});
    auto attachedReopened = model(attachedBytes); QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(attachedReopened));
    const auto attachedGraph = std::get<Projection>(std::get<std::unique_ptr<Model>>(attachedReopened)->inspect(query, 0));
    const auto attachedView = std::find_if(attachedGraph.presentations.cbegin(), attachedGraph.presentations.cend(), [](const auto &p) { return p.kind == "InheritView"; });
    QVERIFY(attachedView != attachedGraph.presentations.cend());
    QCOMPARE(attachedView->route, (QVector<WorldPoint>{{200, 100}, {600, 100}}));
    for (bool ambiguous : {false, true}) {
        auto invalid = source;
        const QByteArray cView = "(object ClassView \"Class\" \"Domain::C\" @3 quidu \"640000000004\" location (700, 100) width 200 height 100)";
        if (ambiguous) { auto duplicate = cView; duplicate.replace("@3", "@5"); invalid.replace(cView, cView + "\n" + duplicate); }
        else invalid.replace(cView, "");
        auto unsafe = model(invalid);
        QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(unsafe));
        auto denied = std::get<std::unique_ptr<Model>>(unsafe)->stage({reconnect});
        QVERIFY(std::holds_alternative<WorkspaceError>(denied));
        QCOMPARE(std::get<UnitBytes>(std::get<std::unique_ptr<Model>>(unsafe)->serialize()).value(UnitId{"main"}), invalid);
    }
    // A writable semantic relationship must not update an appearance in a read-only unit.
    auto mainSource = source.left(source.indexOf("\n logical_presentations")) + ")\n";
    const QByteArray diagramSource = "(object Class_Category \"Shared\" quid \"640000000007\"" +
        source.mid(source.indexOf("\n logical_presentations"));
    auto mainParsed = PetalDocument::parse(mainSource), diagramParsed = PetalDocument::parse(diagramSource);
    QVERIFY(std::holds_alternative<PetalDocument>(mainParsed));
    QVERIFY(std::holds_alternative<PetalDocument>(diagramParsed));
    auto readOnly = Model::fromDocuments({{UnitId{"main"}, "main.mdl", std::get<PetalDocument>(mainParsed), true},
        {UnitId{"views"}, "views.cat", std::get<PetalDocument>(diagramParsed), false}});
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(readOnly));
    auto denied = std::get<std::unique_ptr<Model>>(readOnly)->stage({reconnect});
    QVERIFY(std::holds_alternative<WorkspaceError>(denied));
    QCOMPARE(std::get<WorkspaceError>(denied).code, ErrorCode::AccessDenied);
    auto geometryDenied = std::get<std::unique_ptr<Model>>(readOnly)->stage({Command{SetGeometry{PresentationId{"views#640000000006@1"}, {200, 200, 400, 200}}}});
    QVERIFY(std::holds_alternative<WorkspaceError>(geometryDenied));
    QCOMPARE(std::get<WorkspaceError>(geometryDenied).code, ErrorCode::AccessDenied);
}

void ProjectionTests::controlledDocumentRootCannotBeDeleted() {
    const QByteArray mainSource = "(object Petal version 50 charSet 0)\n"
        "(object Class_Category \"Domain\" quid \"650000000001\" logical_models (list unit_reference_list"
        " (object Class_Category \"Shared\" quid \"650000000003\" is_unit TRUE is_loaded FALSE file_name \"shared.cat\")))\n";
    const QByteArray sharedSource = "(object Class_Category \"Shared\" quid \"650000000003\" logical_models (list unit_reference_list"
        " (object Class \"Leaf\" quid \"650000000004\")))\n";
    auto mainParsed = PetalDocument::parse(mainSource), sharedParsed = PetalDocument::parse(sharedSource);
    QVERIFY(std::holds_alternative<PetalDocument>(mainParsed)); QVERIFY(std::holds_alternative<PetalDocument>(sharedParsed));
    auto opened = Model::fromDocuments({{UnitId{"main"}, "main.mdl", std::get<PetalDocument>(mainParsed), true},
        {UnitId{"shared"}, "shared.cat", std::get<PetalDocument>(sharedParsed), true}});
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
    auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
    auto denied = workspace.stage({Command{DeleteElement{ElementId{"650000000003"}, {"650000000004"}}}});
    QVERIFY(std::holds_alternative<WorkspaceError>(denied));
    QCOMPARE(std::get<WorkspaceError>(denied).code, ErrorCode::UnsafeRewrite);
    const auto after = std::get<UnitBytes>(workspace.serialize());
    QCOMPARE(after.value(UnitId{"main"}), mainSource); QCOMPARE(after.value(UnitId{"shared"}), sharedSource);
    const QByteArray propertyRoot = "(object Petal version 50 charSet 0)\n"
        "(object Design \"Design\" quid \"650000000000\" root_category"
        " (object Class_Category \"Domain\" quid \"650000000001\"))\n";
    auto propertyOwned = model(propertyRoot);
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(propertyOwned));
    auto &propertyWorkspace = *std::get<std::unique_ptr<Model>>(propertyOwned);
    denied = propertyWorkspace.stage({Command{DeleteElement{ElementId{"650000000001"}, {}}}});
    QVERIFY(std::holds_alternative<WorkspaceError>(denied));
    QCOMPARE(std::get<WorkspaceError>(denied).code, ErrorCode::UnsafeRewrite);
    QCOMPARE(std::get<UnitBytes>(propertyWorkspace.serialize()).value(UnitId{"main"}), propertyRoot);
}

void ProjectionTests::typedScalarEditsRespectNativeBinding_data() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QString>("key");
    QTest::newRow("attribute") << QString("ClassAttribute") << QString("type");
    QTest::newRow("parameter") << QString("Parameter") << QString("type");
    QTest::newRow("operation") << QString("Operation") << QString("result");
}

void ProjectionTests::typedScalarEditsRespectNativeBinding() {
    QFETCH(QString, kind); QFETCH(QString, key);
    const QByteArray object = "(object " + kind.toLatin1() + " \"member\" quid \"700000000003\" "
        + key.toLatin1() + " \"Order\" quidu \"700000000002\")";
    QByteArray members;
    if (kind == "ClassAttribute") members = "class_attributes (list class_attribute_list " + object + ')';
    else if (kind == "Operation") members = "operations (list Operations " + object + ')';
    else members = "operations (list Operations (object Operation \"method\" quid \"700000000004\" parameters (list Parameters " + object + ")))";
    const QByteArray source = "(object Petal version 50 charSet 0)\n"
        "(object Class_Category \"Domain\" quid \"700000000001\" logical_models (list unit_reference_list"
        " (object Class \"Order\" quid \"700000000002\" " + members + ")))\n";
    auto opened = model(source); QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
    auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
    auto denied = workspace.stage({Command{SetProperty{ElementId{"700000000003"}, key, QString("double")}}});
    QVERIFY(std::holds_alternative<WorkspaceError>(denied));
    QCOMPARE(std::get<WorkspaceError>(denied).code, ErrorCode::UnsafeRewrite);
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), source);
    auto qualified = workspace.stage({Command{SetProperty{ElementId{"700000000003"}, key, QString("Domain::Order")}}});
    QVERIFY(std::holds_alternative<ModelDelta>(qualified));
    const auto qualifiedDelta = std::get<ModelDelta>(qualified);
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(qualifiedDelta)));
    const auto qualifiedBytes = std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"});
    auto renamed = workspace.stage({
        Command{RenameElement{ElementId{"700000000002"}, "Invoice"}},
        Command{SetProperty{ElementId{"700000000003"}, key, QString("Domain::Invoice")}}});
    QVERIFY(std::holds_alternative<ModelDelta>(renamed));
    const auto renamedDelta = std::get<ModelDelta>(renamed);
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(renamedDelta)));
    auto reopened = model(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}));
    QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(reopened));
    Query query; query.kind = Query::Kind::ElementsById; query.elements = {ElementId{"700000000003"}};
    auto inspected = std::get<std::unique_ptr<Model>>(reopened)->inspect(query, 0);
    QVERIFY(std::holds_alternative<Projection>(inspected));
    const auto &member = std::get<Projection>(inspected).elements.front();
    QCOMPARE(member.properties.value(key), QString("Domain::Invoice"));
    QCOMPARE(member.properties.value("quidu"), QString("700000000002"));
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(renamedDelta, false)));
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), qualifiedBytes);
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(qualifiedDelta, false)));
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), source);
}

void ProjectionTests::associationViewsPreserveOpaqueChildrenAndReconnect() {
    // Authored boundary fixture following genuine TODOLIST44/QVT50 RoleView grammar.
    const QByteArray source =
        "(object Petal version 44 charSet 0)\r\n"
        "(object Class_Category \"Domain\" quid \"710000000001\" logical_models (list unit_reference_list"
        " (object Class \"A\" quid \"710000000002\") (object Class \"B\" quid \"710000000003\") (object Class \"C\" quid \"710000000004\")"
        " (object Association \"link\" quid \"710000000005\" roles (list role_list"
        " (object Role \"first\" quid \"710000000007\" supplier \"Domain::A\" quidu \"710000000002\")"
        " (object Role \"second\" quid \"710000000008\" supplier \"Domain::B\" quidu \"710000000003\"))))"
        " logical_presentations (list unit_reference_list (object ClassDiagram \"Domain\" quid \"710000000006\" items (list diagram_item_list"
        " (object ClassView \"Class\" \"Domain::A\" @1 quidu \"710000000002\" location (100, 100) width 100 height 100)"
        " (object ClassView \"Class\" \"Domain::B\" @2 quidu \"710000000003\" location (500, 100) width 100 height 100)"
        " (object ClassView \"Class\" \"Domain::C\" @3 quidu \"710000000004\" location (700, 100) width 100 height 100)"
        " (object AssociationViewNew \"link\" @4 location (300, 100) quidu \"710000000005\""
        " roleview_list (list RoleViews"
        " (object RoleView \"second\" @5 Parent_View @4 quidu \"710000000008\" client @4 supplier @2"
        " origin_attachment (300, 100) terminal_attachment (500, 100) vendorRole (\"opaque\" 29))"
        " (object RoleView \"first\" @6 Parent_View @4 quidu \"710000000007\" client @4 supplier @1"
        " vertices (list Points (300, 100) (100, 100))"
        " label (object SegLabel @7 Parent_View @6 label \"+first\" vendorLabel (\"opaque\" 31))))"
        " vendorView (\"opaque\" 33))))))\r\n";
    auto opened = model(source); QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
    auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), source);
    Query query; query.kind = Query::Kind::DiagramById; query.diagram = DiagramId{"710000000006"};
    const auto initial = std::get<Projection>(workspace.inspect(query, 0));
    const auto association = std::find_if(initial.presentations.cbegin(), initial.presentations.cend(), [](const auto &p) { return p.kind == "AssociationViewNew"; });
    QVERIFY(association != initial.presentations.cend());
    QCOMPARE(association->route, (QVector<WorldPoint>{{100, 100}, {300, 100}, {500, 100}}));
    auto moved = workspace.stage({
        Command{ReconnectRelation{RelationId{"710000000005"}, {ElementId{"710000000002"}, ElementId{"710000000004"}}}},
        Command{SetGeometry{association->client, {200, 200, 200, 100}}},
    });
    QVERIFY(std::holds_alternative<ModelDelta>(moved));
    const auto moveDelta = std::get<ModelDelta>(moved);
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(moveDelta)));
    const auto movedBytes = std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"});
    auto movedReopened = model(movedBytes); QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(movedReopened));
    const auto movedGraph = std::get<Projection>(std::get<std::unique_ptr<Model>>(movedReopened)->inspect(query, 0));
    const auto movedAssociation = std::find_if(movedGraph.presentations.cbegin(), movedGraph.presentations.cend(), [](const auto &p) { return p.kind == "AssociationViewNew"; });
    QVERIFY(movedAssociation != movedGraph.presentations.cend());
    QCOMPARE(movedAssociation->route, (QVector<WorldPoint>{{200, 200}, {300, 100}, {700, 100}}));
    auto movedParsed = PetalDocument::parse(movedBytes); QVERIFY(std::holds_alternative<PetalDocument>(movedParsed));
    const auto &movedNative = std::get<PetalDocument>(movedParsed);
    for (int role : movedNative.objectsOfKind("RoleView")) {
        const auto identity = movedNative.property(role, "quidu")->rawValue(movedNative);
        if (identity == ByteView("\"710000000008\"")) {
            QCOMPARE(movedNative.property(role, "supplier")->rawValue(movedNative), ByteView("@3"));
            QCOMPARE(movedNative.property(role, "terminal_attachment")->rawValue(movedNative), ByteView("(700, 100)"));
        } else {
            QCOMPARE(identity, ByteView("\"710000000007\""));
            QCOMPARE(movedNative.property(role, "terminal_attachment")->rawValue(movedNative), ByteView("(200, 200)"));
        }
    }
    QVERIFY(movedBytes.contains("vendorRole (\"opaque\" 29)"));
    QVERIFY(movedBytes.contains("label \"+first\" vendorLabel (\"opaque\" 31)"));
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(moveDelta, false)));
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), source);
    auto staged = workspace.stage({
        Command{ReconnectRelation{RelationId{"710000000005"}, {ElementId{"710000000002"}, ElementId{"710000000004"}}}},
        Command{SetRoute{association->id, {{100, 100}, {400, 150}, {700, 100}}}},
        Command{AddPresentation{"extra", query.diagram, ElementId{"710000000002"}, {800, 200, 100, 100}}},
    });
    QVERIFY(std::holds_alternative<ModelDelta>(staged));
    const auto delta = std::get<ModelDelta>(staged);
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta)));
    auto emitted = workspace.serialize(); QVERIFY(std::holds_alternative<UnitBytes>(emitted));
    const auto bytes = std::get<UnitBytes>(emitted).value(UnitId{"main"});
    for (const auto &opaque : {QByteArray("vendorRole (\"opaque\" 29)"), QByteArray("label \"+first\" vendorLabel (\"opaque\" 31)"), QByteArray("vendorView (\"opaque\" 33)")}) QVERIFY(bytes.contains(opaque));
    auto reopened = model(bytes); QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(reopened));
    const auto after = std::get<Projection>(std::get<std::unique_ptr<Model>>(reopened)->inspect(query, 1));
    const auto view = std::find_if(after.presentations.cbegin(), after.presentations.cend(), [](const auto &p) { return p.kind == "AssociationViewNew"; });
    QVERIFY(view != after.presentations.cend());
    const auto supplier = std::find_if(after.presentations.cbegin(), after.presentations.cend(), [&](const auto &p) { return p.id == view->supplier; });
    QVERIFY(supplier != after.presentations.cend()); QCOMPARE(supplier->element, (ElementId{"710000000004"}));
    QCOMPARE(view->route, (QVector<WorldPoint>{{100, 100}, {400, 150}, {700, 100}}));
    const auto extra = std::find_if(after.presentations.cbegin(), after.presentations.cend(), [&](const auto &p) { return p.id.value == delta.newIds.value("extra"); });
    QVERIFY(extra != after.presentations.cend()); QCOMPARE(extra->localLabel, quint64(8));
    QVERIFY(std::holds_alternative<std::monostate>(workspace.applyDelta(delta, false)));
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), source);
}

void ProjectionTests::mixedProfilesAreRejectedWithoutRewritingUnits() {
    for (int version : {44, 50}) {
        auto root = PetalDocument::parse("(object Petal version " + QByteArray::number(version) + " charSet 0)\n(object Class \"A\" quid \"720000000001\")");
        auto child = PetalDocument::parse("(object Petal version " + QByteArray::number(version == 44 ? 50 : 44) + " charSet 0)\n(object Class \"B\" quid \"720000000002\")");
        auto mixed = Model::fromDocuments({{UnitId{"root"}, "root.mdl", std::get<PetalDocument>(root), true}, {UnitId{"child"}, "child.cat", std::get<PetalDocument>(child), true}});
        QVERIFY(std::holds_alternative<WorkspaceError>(mixed)); QCOMPARE(std::get<WorkspaceError>(mixed).code, ErrorCode::UnsupportedProfile);
        auto headerless = PetalDocument::parse("(object Class \"B\" quid \"720000000002\")");
        auto inherited = Model::fromDocuments({{UnitId{"root"}, "root.mdl", std::get<PetalDocument>(root), true}, {UnitId{"child"}, "child.cat", std::get<PetalDocument>(headerless), true}});
        QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(inherited));
        const auto bytes = std::get<UnitBytes>(std::get<std::unique_ptr<Model>>(inherited)->serialize());
        QCOMPARE(bytes.value(UnitId{"child"}), std::get<PetalDocument>(headerless).bytes());
    }
}

void ProjectionTests::unsupportedRelationshipsStayOpaqueAndProtectReferences() {
    const QByteArray source = "(object Petal version 44 charSet 0)\n"
        "(object Class_Category \"Domain\" quid \"730000000001\" logical_models (list unit_reference_list"
        " (object Class \"Actor\" quid \"730000000002\") (object UseCase \"Login\" quid \"730000000003\")"
        " (object Association \"uses\" quid \"730000000004\" roles (list role_list"
        " (object Role \"actor\" quid \"730000000005\" quidu \"730000000002\")"
        " (object Role \"usecase\" quid \"730000000006\" quidu \"730000000003\")))))\n";
    auto opened = model(source); QVERIFY(std::holds_alternative<std::unique_ptr<Model>>(opened));
    auto &workspace = *std::get<std::unique_ptr<Model>>(opened);
    const auto graph = std::get<Projection>(workspace.inspect({}, 0));
    QVERIFY(graph.relations.isEmpty());
    QVERIFY(std::any_of(graph.diagnostics.cbegin(), graph.diagnostics.cend(), [](const auto &d) { return d.code == "OpaqueRelationship"; }));
    auto deletion = workspace.stage({Command{DeleteElement{ElementId{"730000000002"}, {}}}});
    QVERIFY(std::holds_alternative<WorkspaceError>(deletion)); QCOMPARE(std::get<WorkspaceError>(deletion).code, ErrorCode::UnsafeRewrite);
    QCOMPARE(std::get<UnitBytes>(workspace.serialize()).value(UnitId{"main"}), source);
}
QTEST_GUILESS_MAIN(ProjectionTests)
#include "tst_projection.moc"
