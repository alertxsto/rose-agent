#include <QtTest>
#include <QTemporaryDir>
#include "core/Workspace.h"
#include "core/Storage.h"

using namespace rose;

class TransactionTests final : public QObject {
    Q_OBJECT
private slots:
    void exactRenameUndoRedoAndRoundtrip();
    void staleAndChangedApprovalNeverMutate();
    void invalidBatchLeavesModelAndRevisionUntouched();
    void createAuthoredNativeRoots();
    void createClassModelWithNativeRelationLayout_data();
    void createClassModelWithNativeRelationLayout();
};

static QByteArray authoredModel() {
    return "(object Petal version 50 charSet 0)\r\n"
           "(object Design \"Logical View\" is_unit TRUE is_loaded TRUE quid \"650000000000\"\r\n"
           " root_category (object Class_Category \"Logical View\" quid \"650000000001\"\r\n"
           " logical_models (list unit_reference_list\r\n"
           " (object Class \"Order\" quid \"650000000002\" documentation \"(object @7)\" vendorExtension (\"foreign\" 9)))\r\n"
           " logical_presentations (list unit_reference_list\r\n"
           " (object ClassDiagram \"First\" quid \"650000000010\" items (list diagram_item_list\r\n"
           " (object ClassView \"Class\" \"Logical View::Order\" @1 quidu \"650000000002\" location (100, 200) width 120 height 80)))\r\n"
           " (object ClassDiagram \"Second\" quid \"650000000011\" items (list diagram_item_list\r\n"
           " (object ClassView \"Class\" \"Logical View::Order\" @1 quidu \"650000000002\" location (300, 400) width 140 height 90))))))\r\n";
}
static AccessPolicy policy(const QTemporaryDir &dir) { return {{dir.path()}, {}, "ASCII", true}; }
static void put(const QString &path, const QByteArray &bytes) {
    QFile f(path); if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size()) qFatal("fixture write failed");
}
static ElementRecord order(Workspace &w) {
    Query q; q.kind = Query::Kind::Search; q.search = "Order"; q.limit = 100;
    auto p = w.inspect(q); if (!std::holds_alternative<Projection>(p)) qFatal("inspect failed");
    for (const auto &e : std::get<Projection>(p).elements) if (e.kind == "Class") return e;
    qFatal("authored class absent");
}
static Proposal proposeRename(Workspace &w, const ElementId &id, QString name) {
    auto p = w.propose(w.revision(), {Command{RenameElement{id, std::move(name)}}});
    if (!std::holds_alternative<Proposal>(p)) qFatal("propose failed");
    return std::get<Proposal>(p);
}
void TransactionTests::exactRenameUndoRedoAndRoundtrip() {
    QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath("model.mdl");
    const auto source = authoredModel(); put(path, source);
    auto opened = Workspace::open(path, policy(dir)); QVERIFY(std::holds_alternative<std::unique_ptr<Workspace>>(opened));
    auto w = std::move(std::get<std::unique_ptr<Workspace>>(opened)); const auto original = order(*w);
    auto p = proposeRename(*w, original.id, "Invoice");
    auto applied = w->apply({p.id, p.baseRevision, p.digest}); QVERIFY(std::holds_alternative<Applied>(applied));
    QCOMPARE(w->revision(), Revision(1)); QVERIFY(w->dirty());
    Query tree; tree.limit = 1000; auto view = w->inspect(tree); QVERIFY(std::holds_alternative<Projection>(view));
    bool renamed = false;
    for (const auto &e : std::get<Projection>(view).elements) if (e.id == original.id) { QCOMPARE(e.name, QString("Invoice")); renamed = true; }
    QVERIFY(renamed);
    QVERIFY(std::holds_alternative<Applied>(w->undo())); QCOMPARE(w->revision(), Revision(2));
    QCOMPARE(order(*w), original);
    QVERIFY(std::holds_alternative<SaveReceipt>(w->save()));
    QFile restored(path); QVERIFY(restored.open(QIODevice::ReadOnly)); QCOMPARE(restored.readAll(), source); restored.close();
    QVERIFY(std::holds_alternative<Applied>(w->redo())); QCOMPARE(w->revision(), Revision(3));
    QVERIFY(std::holds_alternative<SaveReceipt>(w->save())); QVERIFY(!w->dirty());
    auto reopened = Workspace::open(path, policy(dir)); QVERIFY(std::holds_alternative<std::unique_ptr<Workspace>>(reopened));
    Query byId; byId.kind = Query::Kind::ElementsById; byId.elements = {original.id};
    auto after = std::get<std::unique_ptr<Workspace>>(reopened)->inspect(byId); QVERIFY(std::holds_alternative<Projection>(after));
    QCOMPARE(std::get<Projection>(after).elements.front().name, QString("Invoice"));
    QCOMPARE(std::get<Projection>(after).elements.front().id, original.id);
    QFile saved(path); QVERIFY(saved.open(QIODevice::ReadOnly)); auto bytes = saved.readAll();
    QVERIFY(bytes.contains("vendorExtension (\"foreign\" 9)")); QVERIFY(bytes.contains("documentation \"(object @7)\""));
}
void TransactionTests::staleAndChangedApprovalNeverMutate() {
    QTemporaryDir dir; const auto path = dir.filePath("model.mdl"); put(path, authoredModel());
    auto opened = Workspace::open(path, policy(dir)); QVERIFY(std::holds_alternative<std::unique_ptr<Workspace>>(opened));
    auto w = std::move(std::get<std::unique_ptr<Workspace>>(opened)); const auto id = order(*w).id;
    auto a = proposeRename(*w, id, "A"); auto b = proposeRename(*w, id, "B");
    auto changed = a.digest; changed[0] = char(changed[0] ^ 1);
    auto denied = w->apply({a.id, a.baseRevision, changed}); QVERIFY(std::holds_alternative<WorkspaceError>(denied));
    QCOMPARE(std::get<WorkspaceError>(denied).code, ErrorCode::StaleApproval); QCOMPARE(w->revision(), Revision(0));
    QVERIFY(std::holds_alternative<Applied>(w->apply({a.id, a.baseRevision, a.digest})));
    auto stale = w->apply({b.id, b.baseRevision, b.digest}); QVERIFY(std::holds_alternative<WorkspaceError>(stale));
    QCOMPARE(std::get<WorkspaceError>(stale).code, ErrorCode::StaleApproval); QCOMPARE(w->revision(), Revision(1));
    QVERIFY(std::holds_alternative<Applied>(w->undo())); QCOMPARE(order(*w).name, QString("Order"));
    auto c = proposeRename(*w, id, "C"); QVERIFY(std::holds_alternative<std::monostate>(w->reject(c.id)));
    auto rejected = w->apply({c.id, c.baseRevision, c.digest}); QVERIFY(std::holds_alternative<WorkspaceError>(rejected));
    QCOMPARE(w->revision(), Revision(2));
}
void TransactionTests::invalidBatchLeavesModelAndRevisionUntouched() {
    QTemporaryDir dir; const auto path = dir.filePath("model.mdl"); put(path, authoredModel());
    auto opened = Workspace::open(path, policy(dir)); QVERIFY(std::holds_alternative<std::unique_ptr<Workspace>>(opened));
    auto w = std::move(std::get<std::unique_ptr<Workspace>>(opened)); const auto original = order(*w);
    auto result = w->propose(0, {Command{RenameElement{original.id, "Invoice"}}, Command{RenameElement{ElementId{"missing"}, "Bad"}}});
    QVERIFY(std::holds_alternative<WorkspaceError>(result)); QCOMPARE(w->revision(), Revision(0));
    QCOMPARE(order(*w), original); QVERIFY(!w->dirty());
    QVERIFY(std::holds_alternative<WorkspaceError>(w->undo()));
}
void TransactionTests::createAuthoredNativeRoots() {
    QTemporaryDir dir; auto made = Workspace::create(dir.filePath("fresh.mdl"), {}, policy(dir));
    QVERIFY(std::holds_alternative<std::unique_ptr<Workspace>>(made)); auto w = std::move(std::get<std::unique_ptr<Workspace>>(made));
    QVERIFY(w->dirty()); QVERIFY(std::holds_alternative<SaveReceipt>(w->save()));
    QFile f(w->path()); QVERIFY(f.open(QIODevice::ReadOnly)); auto parsed = PetalDocument::parse(f.readAll());
    QVERIFY(std::holds_alternative<PetalDocument>(parsed)); const auto &d = std::get<PetalDocument>(parsed);
    QCOMPARE(d.objectsOfKind("Design").size(), 1); const auto design = d.objectsOfKind("Design").front();
    QVERIFY(d.property(design, "defaults")); QVERIFY(d.property(design, "root_category"));
    QVERIFY(d.property(design, "root_usecase_package")); QVERIFY(d.property(design, "root_subsystem"));
    QVERIFY(d.property(design, "process_structure"));
}
void TransactionTests::createClassModelWithNativeRelationLayout_data() {
    QTest::addColumn<int>("version");
    QTest::newRow("bundled-44-class-profile") << 44;
    QTest::newRow("public-50-class-profile") << 50;
}
void TransactionTests::createClassModelWithNativeRelationLayout() {
    QFETCH(int, version);
    QTemporaryDir dir;
    auto made = Workspace::create(dir.filePath("generated.mdl"), {version, "ASCII"}, policy(dir));
    QVERIFY(std::holds_alternative<std::unique_ptr<Workspace>>(made));
    auto w = std::move(std::get<std::unique_ptr<Workspace>>(made));
    const auto initial = std::get<Projection>(w->inspect({}));
    ElementId root;
    for (const auto &e : initial.elements) if (e.kind == "Class_Category" && e.name == "Logical View") root = e.id;
    QVERIFY(!root.isEmpty());
    const QVector<Command> commands{
        Command{CreateElement{"a", "Class", "Order", root, {}}},
        Command{CreateElement{"b", "Class", "Entity", root, {}}},
        Command{CreateElement{"field", "ClassAttribute", "total", ElementId{"a"}, {}}},
        Command{SetProperty{ElementId{"field"}, "type", QString("double")}},
        Command{CreateElement{"method", "Operation", "submit", ElementId{"a"}, {}}},
        Command{SetProperty{ElementId{"method"}, "result", QString("bool")}},
        Command{CreateElement{"parameter", "Parameter", "urgent", ElementId{"method"}, {}}},
        Command{SetProperty{ElementId{"parameter"}, "type", QString("bool")}},
        Command{SetProperty{ElementId{"field"}, "exportControl", QString("Private")}},
        Command{CreateRelation{"association", "Association", "owns", root, {ElementId{"a"}, ElementId{"b"}}, {}}},
        Command{CreateRelation{"inheritance", "Inheritance_Relationship", {}, ElementId{"a"}, {ElementId{"a"}, ElementId{"b"}}, {}}},
        Command{CreateDiagram{"diagram", "ClassDiagram", "Domain", root, {}}},
        Command{AddPresentation{"av", DiagramId{"diagram"}, ElementId{"a"}, {100, 100, 250, 160}}},
        Command{AddPresentation{"bv", DiagramId{"diagram"}, ElementId{"b"}, {600, 100, 250, 160}}},
        Command{AddPresentation{"assocv", DiagramId{"diagram"}, RelationId{"association"}, {}}},
        Command{SetRoute{PresentationId{"assocv"}, {{100, 100}, {350, 100}, {600, 100}}}},
        Command{AddPresentation{"inheritv", DiagramId{"diagram"}, RelationId{"inheritance"}, {}}},
    };
    auto proposed = w->propose(0, commands);
    QVERIFY(std::holds_alternative<Proposal>(proposed));
    QCOMPARE(w->revision(), Revision(0));
    QVERIFY(!QFile::exists(w->path())); // Review and apply do not implicitly save.
    const auto proposal = std::get<Proposal>(proposed);
    auto reviewed = [&](const QString &client, const QString &field) -> QString {
        const auto id = proposal.newIds.value(client);
        for (const auto &entry : proposal.diff) if (entry.objectId == id && entry.field == field) return entry.after;
        return "<missing>";
    };
    QCOMPARE(reviewed("a", "name"), QString("Order"));
    QCOMPARE(reviewed("field", "name"), QString("total"));
    QCOMPARE(reviewed("field", "owner"), proposal.newIds.value("a"));
    QCOMPARE(reviewed("field", "type"), QString("double"));
    QCOMPARE(reviewed("field", "type.type"), QString("string"));
    QCOMPARE(reviewed("field", "exportControl"), QString("Private"));
    QCOMPARE(reviewed("method", "result"), QString("bool"));
    QCOMPARE(reviewed("parameter", "name"), QString("urgent"));
    QCOMPARE(reviewed("parameter", "owner"), proposal.newIds.value("method"));
    QCOMPARE(reviewed("parameter", "type"), QString("bool"));
    QCOMPARE(reviewed("association", "name"), QString("owns"));
    QCOMPARE(reviewed("association", "endpoints"), proposal.newIds.value("a") + ',' + proposal.newIds.value("b"));
    QCOMPARE(reviewed("diagram", "name"), QString("Domain"));
    QCOMPARE(reviewed("diagram", "owner"), root.value);
    QCOMPARE(reviewed("av", "geometry"), QString("100,100,250,160"));
    QCOMPARE(reviewed("av", "element"), proposal.newIds.value("a"));
    QCOMPARE(reviewed("av", "IncludeAttribute"), QString("TRUE"));
    QCOMPARE(reviewed("assocv", "relation"), proposal.newIds.value("association"));
    QCOMPARE(reviewed("assocv", "client"), proposal.newIds.value("av"));
    QCOMPARE(reviewed("assocv", "supplier"), proposal.newIds.value("bv"));
    QCOMPARE(reviewed("assocv", "route"), QString("(100, 100) (350, 100) (600, 100)"));
    QCOMPARE(reviewed("inheritv", "route"), QString("(225, 100) (475, 100)"));
    QVERIFY(std::holds_alternative<Applied>(w->apply({proposal.id, proposal.baseRevision, proposal.digest})));
    QVERIFY(!QFile::exists(w->path()));
    QVERIFY(std::holds_alternative<SaveReceipt>(w->save()));
    QFile file(w->path()); QVERIFY(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    auto parsed = PetalDocument::parse(bytes);
    QVERIFY(std::holds_alternative<PetalDocument>(parsed));
    const auto &native = std::get<PetalDocument>(parsed);
    QCOMPARE(native.property(native.objectsOfKind("Petal").front(), "version")->rawValue(native), ByteView(QByteArray::number(version)));
    QCOMPARE(native.objectsOfKind("RoleView").size(), 2);
    QCOMPARE(native.objectsOfKind("AssociationViewNew").size(), 1);
    QCOMPARE(native.objectsOfKind("InheritView").size(), 1);
    QSet<QString> nativeIds;
    for (int n = 0; n < native.nodes().size(); ++n) if (const auto quid = native.property(n, "quid")) {
        const auto id = quid->rawValue(native).toByteArray();
        QVERIFY(!nativeIds.contains(QString::fromLatin1(id))); nativeIds.insert(QString::fromLatin1(id));
    }
    auto opened = Workspace::open(w->path(), policy(dir));
    QVERIFY(std::holds_alternative<std::unique_ptr<Workspace>>(opened));
    auto &reopened = *std::get<std::unique_ptr<Workspace>>(opened);
    Query query; query.kind = Query::Kind::DiagramById; query.diagram = DiagramId{proposal.newIds.value("diagram")};
    const auto result = std::get<Projection>(reopened.inspect(query));
    QCOMPARE(result.presentations.size(), 4);
    QCOMPARE(result.relations.size(), 2);
    QCOMPARE(result.elements.size(), 5);
    for (const auto &e : result.elements) {
        if (e.id.value == proposal.newIds.value("field")) QCOMPARE(e.properties.value("type"), QString("double"));
        if (e.id.value == proposal.newIds.value("method")) QCOMPARE(e.properties.value("result"), QString("bool"));
        if (e.id.value == proposal.newIds.value("parameter")) {
            QCOMPARE(e.name, QString("urgent")); QCOMPARE(e.owner.value, proposal.newIds.value("method"));
        }
    }
    for (const auto &p : result.presentations) if (p.kind == "AssociationViewNew") {
        QCOMPARE(p.client.value, proposal.newIds.value("av")); QCOMPARE(p.supplier.value, proposal.newIds.value("bv"));
        QCOMPARE(p.route, (QVector<WorldPoint>{{100, 100}, {350, 100}, {600, 100}}));
    }
    for (const auto &p : result.presentations) if (p.kind == "InheritView") {
        QCOMPARE(p.route, (QVector<WorldPoint>{{225, 100}, {475, 100}}));
    }
    Query members; members.kind = Query::Kind::ElementsById;
    members.elements = {ElementId{proposal.newIds.value("field")}, ElementId{proposal.newIds.value("method")}};
    const auto semantics = std::get<Projection>(reopened.inspect(members));
    QCOMPARE(semantics.elements[0].properties.value("type"), QString("double"));
    QCOMPARE(semantics.elements[1].properties.value("result"), QString("bool"));
}
QTEST_GUILESS_MAIN(TransactionTests)
#include "tst_transactions.moc"
