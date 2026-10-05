#include <QtTest>
#include <QProcess>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDir>
#include <QMap>

class CliTests final : public QObject {
    Q_OBJECT
    QString program() const { return QCoreApplication::applicationDirPath() + "/rose-cli"
#ifdef Q_OS_WIN
        ".exe"
#endif
        ; }
    QJsonObject request(QProcess &process, const QJsonObject &value) {
        process.write(QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n');
        QByteArray line;
        QElapsedTimer timer; timer.start();
        while (!process.canReadLine() && timer.elapsed() < 10000) process.waitForReadyRead(100);
        if (process.canReadLine()) line = process.readLine();
        return QJsonDocument::fromJson(line).object();
    }
    void start(QProcess &process, const QString &root) {
        process.start(program(), {"session", "--allow-root", root});
    }
private slots:
    void approvedRenameUndoSaveAndReopen() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString path = dir.filePath("model.mdl");
        QFile fixture(path); QVERIFY(fixture.open(QIODevice::WriteOnly));
        const QByteArray source = "(object Petal version 50 charSet 0)\r\n"
            "(object Design \"Logical View\" is_unit TRUE is_loaded TRUE quid \"650000000000\"\r\n"
            " root_category (object Class_Category \"Logical View\" quid \"650000000001\"\r\n"
            " logical_models (list unit_reference_list\r\n"
            " (object Class \"Order\" quid \"650000000002\" vendorExtension (\"UnknownKind\" 201)))))\r\n";
        QCOMPARE(fixture.write(source), source.size()); fixture.close();
        QProcess process; start(process, dir.path()); QVERIFY(process.waitForStarted());
        auto opened = request(process, {{"id", "open"}, {"method", "open"}, {"params", QJsonObject{{"file", path}}}});
        QVERIFY2(opened.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(opened).toJson())));
        const auto projection = opened.value("result").toObject();
        QString id;
        for (const auto &value : projection.value("elements").toArray()) {
            auto record = value.toObject(); if (record.value("name") == "Order") id = record.value("id").toString();
        }
        QVERIFY(!id.isEmpty());
        QJsonObject proposeParams{{"baseRevision", projection.value("revision")}, {"commands", QJsonArray{QJsonObject{{"type", "renameElement"}, {"id", id}, {"name", "Invoice"}}}}};
        auto proposed = request(process, {{"id", "propose"}, {"method", "propose"}, {"params", proposeParams}});
        QVERIFY(proposed.value("ok").toBool());
        auto pending = proposed.value("result").toObject();
        auto before = request(process, {{"id", "before"}, {"method", "inspect"}, {"params", QJsonObject{{"kind", "elementsById"}, {"elements", QJsonArray{id}}}}});
        QCOMPARE(before.value("result").toObject().value("elements").toArray().first().toObject().value("name").toString(), QString("Order"));
        QJsonObject approval{{"id", pending.value("id")}, {"baseRevision", pending.value("baseRevision")}, {"digest", pending.value("digest")}};
        auto alteredApproval = approval; alteredApproval.insert("digest", QString(64, '0'));
        auto denied = request(process, {{"id", "altered"}, {"method", "apply"}, {"params", alteredApproval}});
        QVERIFY(!denied.value("ok").toBool());
        QCOMPARE(denied.value("error").toObject().value("code").toString(), QString("staleApproval"));
        QVERIFY(request(process, {{"id", "apply"}, {"method", "apply"}, {"params", approval}}).value("ok").toBool());
        QFile unchanged(path); QVERIFY(unchanged.open(QIODevice::ReadOnly)); QCOMPARE(unchanged.readAll(), source); unchanged.close();
        QVERIFY(request(process, {{"id", "undo"}, {"method", "undo"}, {"params", QJsonObject{}}}).value("ok").toBool());
        auto undone = request(process, {{"id", "undone"}, {"method", "inspect"}, {"params", QJsonObject{{"kind", "elementsById"}, {"elements", QJsonArray{id}}}}});
        QCOMPARE(undone.value("result").toObject().value("elements").toArray().first().toObject().value("name").toString(), QString("Order"));
        QVERIFY(request(process, {{"id", "redo"}, {"method", "redo"}, {"params", QJsonObject{}}}).value("ok").toBool());
        QVERIFY(request(process, {{"id", "undoAgain"}, {"method", "undo"}, {"params", QJsonObject{}}}).value("ok").toBool());
        auto current = request(process, {{"id", "current"}, {"method", "inspect"}, {"params", QJsonObject{{"kind", "elementsById"}, {"elements", QJsonArray{id}}}}});
        QCOMPARE(current.value("result").toObject().value("elements").toArray().first().toObject().value("name").toString(), QString("Order"));
        proposeParams.insert("baseRevision", current.value("revision"));
        auto reproposed = request(process, {{"id", "repropose"}, {"method", "propose"}, {"params", proposeParams}});
        QVERIFY(reproposed.value("ok").toBool());
        auto newPending = reproposed.value("result").toObject();
        QJsonObject newApproval{{"id", newPending.value("id")}, {"baseRevision", newPending.value("baseRevision")}, {"digest", newPending.value("digest")}};
        QVERIFY(request(process, {{"id", "reapply"}, {"method", "apply"}, {"params", newApproval}}).value("ok").toBool());
        QVERIFY(request(process, {{"id", "save"}, {"method", "save"}, {"params", QJsonObject{}}}).value("ok").toBool());
        QVERIFY(request(process, {{"id", "close"}, {"method", "close"}, {"params", QJsonObject{}}}).value("ok").toBool());
        process.closeWriteChannel(); QVERIFY(process.waitForFinished()); QCOMPARE(process.exitCode(), 0);
        QProcess reopened;
        reopened.start(program(), {"inspect", "--file", path, "--allow-root", dir.path()});
        QVERIFY(reopened.waitForFinished()); QCOMPARE(reopened.exitCode(), 0);
        const auto after = QJsonDocument::fromJson(reopened.readAllStandardOutput()).object();
        bool found = false;
        for (const auto &value : after.value("elements").toArray()) {
            auto record = value.toObject(); if (record.value("id") == id) { found = true; QCOMPARE(record.value("name").toString(), QString("Invoice")); }
        }
        QVERIFY(found);
        QFile saved(path); QVERIFY(saved.open(QIODevice::ReadOnly)); QVERIFY(saved.readAll().contains("vendorExtension (\"UnknownKind\" 201)"));
    }
    void malformedRequestsAndRoundtripPreserveSource() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString path = dir.filePath("source.mdl");
        QFile source(path); QVERIFY(source.open(QIODevice::WriteOnly));
        const QByteArray bytes = "(object Petal version 50 charSet 0)\n(object Class \"Item\" quid \"47209F1F003E\")\n";
        QCOMPARE(source.write(bytes), bytes.size()); source.close();
        QProcess roundtrip; roundtrip.start(program(), {"roundtrip", "--file", path, "--allow-root", dir.path()});
        QVERIFY(roundtrip.waitForFinished()); QCOMPARE(roundtrip.exitCode(), 0);
        QVERIFY(source.open(QIODevice::ReadOnly)); QCOMPARE(source.readAll(), bytes); source.close();
        const auto receipt = QJsonDocument::fromJson(roundtrip.readAllStandardOutput()).object();
        QVERIFY(receipt.value("path").toString() != path); QVERIFY(QFile::exists(receipt.value("path").toString()));
        QProcess process; start(process, dir.path()); QVERIFY(process.waitForStarted());
        process.write("not json\n"); QVERIFY(process.waitForReadyRead());
        auto malformed = QJsonDocument::fromJson(process.readLine()).object(); QVERIFY(!malformed.value("ok").toBool());
        auto unknown = request(process, {{"id", "bad"}, {"method", "open"}, {"params", QJsonObject{{"file", path}, {"typo", true}}}});
        QCOMPARE(unknown.value("id").toString(), QString("bad")); QVERIFY(!unknown.value("ok").toBool());
        auto valid = request(process, {{"id", "good"}, {"method", "open"}, {"params", QJsonObject{{"file", path}}}}); QVERIFY(valid.value("ok").toBool());
        process.closeWriteChannel(); QVERIFY(process.waitForFinished());
    }
    void inspectionOutputHonorsAllowedRoots() {
        QTemporaryDir allowed, outside; QVERIFY(allowed.isValid()); QVERIFY(outside.isValid());
        const auto path = allowed.filePath("source.mdl"), destination = outside.filePath("inspection.json");
        QFile fixture(path); QVERIFY(fixture.open(QIODevice::WriteOnly));
        const QByteArray source = "(object Petal version 50 charSet 0)\n(object Class \"Item\" quid \"47209F1F003E\")\n";
        QCOMPARE(fixture.write(source), source.size()); fixture.close();
        QProcess process; process.start(program(), {"inspect", "--file", path, "--output", destination, "--allow-root", allowed.path()});
        QVERIFY(process.waitForFinished()); QCOMPARE(process.exitCode(), 1);
        QCOMPARE(QJsonDocument::fromJson(process.readAllStandardError()).object().value("code").toString(), QString("accessDenied"));
        QVERIFY(!QFile::exists(destination));
        const auto inside = allowed.filePath("inspection.json");
        process.start(program(), {"inspect", "--file", path, "--output", inside, "--allow-root", allowed.path()});
        QVERIFY(process.waitForFinished()); QCOMPARE(process.exitCode(), 0);
        QFile inspected(inside); QVERIFY(inspected.open(QIODevice::ReadOnly));
        auto projection = QJsonDocument::fromJson(inspected.readAll()).object();
        QString name;
        for (const auto &element : projection.value("elements").toArray()) if (element.toObject().value("kind") == "Class") name = element.toObject().value("name").toString();
        QCOMPARE(name, QString("Item"));
    }
    void readOnlyAndRejectedApprovalDoNotSave() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path = dir.filePath("created.mdl");
        QProcess process; start(process, dir.path()); QVERIFY(process.waitForStarted());
        auto created = request(process, {{"id", "create"}, {"method", "create"}, {"params", QJsonObject{{"file", path}}}});
        QVERIFY(created.value("ok").toBool()); QVERIFY(!QFile::exists(path));
        QVERIFY(request(process, {{"id", "save"}, {"method", "save"}, {"params", QJsonObject{}}}).value("ok").toBool());
        QVERIFY(request(process, {{"id", "close"}, {"method", "close"}, {"params", QJsonObject{}}}).value("ok").toBool());
        process.closeWriteChannel(); QVERIFY(process.waitForFinished());
        QFile saved(path); QVERIFY(saved.open(QIODevice::ReadOnly)); const auto bytes = saved.readAll(); saved.close();
        process.start(program(), {"session", "--file", path, "--allow-root", dir.path(), "--read-only"}); QVERIFY(process.waitForStarted());
        auto inspected = request(process, {{"id", "inspect"}, {"method", "inspect"}, {"params", QJsonObject{{"kind", "modelTree"}}}});
        QVERIFY(inspected.value("ok").toBool());
        auto denied = request(process, {{"id", "save"}, {"method", "save"}, {"params", QJsonObject{}}});
        QVERIFY(!denied.value("ok").toBool()); QCOMPARE(denied.value("error").toObject().value("code").toString(), QString("accessDenied"));
        process.closeWriteChannel(); QVERIFY(process.waitForFinished());
        QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(), bytes); saved.close();
        process.start(program(), {"session", "--file", path, "--allow-root", dir.path()}); QVERIFY(process.waitForStarted());
        QString root;
        auto tree = request(process, {{"id", "inspect"}, {"method", "inspect"}, {"params", QJsonObject{{"kind", "modelTree"}}}});
        for (const auto &element : tree.value("result").toObject().value("elements").toArray()) if (element.toObject().value("kind") == "Class_Category") root = element.toObject().value("id").toString();
        QVERIFY(!root.isEmpty());
        auto proposed = request(process, {{"id", "propose"}, {"method", "propose"}, {"params", QJsonObject{{"baseRevision", tree.value("revision")}, {"commands", QJsonArray{QJsonObject{{"type", "renameElement"}, {"id", root}, {"name", "Renamed"}}}}}}});
        QVERIFY(proposed.value("ok").toBool()); auto pending = proposed.value("result").toObject();
        auto rejected = request(process, {{"id", "reject"}, {"method", "reject"}, {"params", QJsonObject{{"id", pending.value("id")}}}});
        QVERIFY(rejected.value("ok").toBool()); QCOMPARE(rejected.value("result").toObject().value("type").toString(), QString("rejected"));
        auto refused = request(process, {{"id", "apply"}, {"method", "apply"}, {"params", QJsonObject{{"id", pending.value("id")}, {"baseRevision", pending.value("baseRevision")}, {"digest", pending.value("digest")}}}});
        QVERIFY(!refused.value("ok").toBool()); QCOMPARE(refused.value("revision").toString(), tree.value("revision").toString());
        process.closeWriteChannel(); QVERIFY(process.waitForFinished());
        QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(), bytes);
    }
    void createdClassGraphPersistsOwnershipAndPresentationIdentity() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto path = dir.filePath("billing.mdl");
        QProcess process; start(process, dir.path()); QVERIFY(process.waitForStarted());
        const auto created = request(process, {{"id", "create"}, {"method", "create"}, {"params", QJsonObject{{"file", path}}}});
        QVERIFY(created.value("ok").toBool());
        const auto initial = created.value("result").toObject();
        QString owner;
        for (const auto &value : initial.value("elements").toArray()) {
            const auto record = value.toObject();
            if (record.value("kind") == "Class_Category" && record.value("name") == "Logical View") owner = record.value("id").toString();
        }
        QVERIFY(!owner.isEmpty());
        const QJsonArray commands{
            QJsonObject{{"type", "createElement"}, {"clientId", "invoice"}, {"kind", "Class"}, {"name", "Invoice"}, {"owner", owner}, {"unit", ""}},
            QJsonObject{{"type", "createElement"}, {"clientId", "customer"}, {"kind", "Class"}, {"name", "Customer"}, {"owner", owner}, {"unit", ""}},
            QJsonObject{{"type", "createElement"}, {"clientId", "total"}, {"kind", "ClassAttribute"}, {"name", "total"}, {"owner", "invoice"}, {"unit", ""}},
            QJsonObject{{"type", "createElement"}, {"clientId", "pay"}, {"kind", "Operation"}, {"name", "pay"}, {"owner", "invoice"}, {"unit", ""}},
            QJsonObject{{"type", "setProperty"}, {"id", QJsonObject{{"kind", "element"}, {"value", "total"}}}, {"key", "type"}, {"value", "double"}},
            QJsonObject{{"type", "createRelation"}, {"clientId", "billed-to"}, {"kind", "Association"}, {"name", "billedTo"}, {"owner", owner}, {"endpoints", QJsonArray{"invoice", "customer"}}, {"properties", QJsonObject{}}},
            QJsonObject{{"type", "createDiagram"}, {"clientId", "billing"}, {"kind", "ClassDiagram"}, {"name", "Billing"}, {"owner", owner}, {"unit", ""}},
            QJsonObject{{"type", "addPresentation"}, {"clientId", "invoice-view"}, {"diagram", "billing"}, {"subject", QJsonObject{{"kind", "element"}, {"value", "invoice"}}}, {"geometry", QJsonObject{{"x", 100}, {"y", 200}, {"width", 160}, {"height", 100}}}},
            QJsonObject{{"type", "addPresentation"}, {"clientId", "customer-view"}, {"diagram", "billing"}, {"subject", QJsonObject{{"kind", "element"}, {"value", "customer"}}}, {"geometry", QJsonObject{{"x", 500}, {"y", 200}, {"width", 160}, {"height", 100}}}}
        };
        const auto proposed = request(process, {{"id", "propose"}, {"method", "propose"}, {"params", QJsonObject{{"baseRevision", initial.value("revision")}, {"commands", commands}}}});
        QVERIFY2(proposed.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(proposed).toJson())));
        const auto pending = proposed.value("result").toObject();
        const auto ids = pending.value("newIds").toObject();
        const auto invoiceId = ids.value("invoice").toString(), customerId = ids.value("customer").toString();
        const auto attributeId = ids.value("total").toString(), operationId = ids.value("pay").toString();
        const auto relationId = ids.value("billed-to").toString(), diagramId = ids.value("billing").toString();
        const auto viewId = ids.value("invoice-view").toString();
        QVERIFY(!invoiceId.isEmpty() && !customerId.isEmpty() && !attributeId.isEmpty() && !operationId.isEmpty());
        QVERIFY(!relationId.isEmpty() && !diagramId.isEmpty() && !viewId.isEmpty());
        QVERIFY(!QFile::exists(path));
        const auto before = request(process, {{"id", "before"}, {"method", "inspect"}, {"params", QJsonObject{{"kind", "search"}, {"search", "Invoice"}}}});
        QVERIFY(before.value("result").toObject().value("elements").toArray().isEmpty());
        QVERIFY(request(process, {{"id", "apply"}, {"method", "apply"}, {"params", QJsonObject{{"id", pending.value("id")}, {"baseRevision", pending.value("baseRevision")}, {"digest", pending.value("digest")}}}}).value("ok").toBool());
        QVERIFY(!QFile::exists(path));
        QVERIFY(request(process, {{"id", "save"}, {"method", "save"}, {"params", QJsonObject{}}}).value("ok").toBool());
        QVERIFY(request(process, {{"id", "close"}, {"method", "close"}, {"params", QJsonObject{}}}).value("ok").toBool());
        process.closeWriteChannel(); QVERIFY(process.waitForFinished()); QCOMPARE(process.exitCode(), 0);
        QProcess reopened; reopened.start(program(), {"session", "--file", path, "--allow-root", dir.path()});
        QVERIFY(reopened.waitForStarted());
        const auto queried = request(reopened, {{"id", "graph"}, {"method", "inspect"}, {"params", QJsonObject{{"kind", "diagramById"}, {"diagram", diagramId}}}});
        QVERIFY2(queried.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(queried).toJson())));
        const auto graph = queried.value("result").toObject();
        QMap<QString, QJsonObject> elements;
        const auto all = request(reopened, {{"id", "elements"}, {"method", "inspect"}, {"params", QJsonObject{{"kind", "elementsById"}, {"elements", QJsonArray{invoiceId, customerId, attributeId, operationId}}}}});
        QVERIFY(all.value("ok").toBool());
        for (const auto &value : all.value("result").toObject().value("elements").toArray()) elements.insert(value.toObject().value("id").toString(), value.toObject());
        QCOMPARE(elements.value(invoiceId).value("name").toString(), QString("Invoice"));
        QCOMPARE(elements.value(invoiceId).value("owner").toString(), owner);
        QCOMPARE(elements.value(customerId).value("name").toString(), QString("Customer"));
        QCOMPARE(elements.value(attributeId).value("owner").toString(), invoiceId);
        QCOMPARE(elements.value(attributeId).value("properties").toObject().value("type").toString(), QString("double"));
        QCOMPARE(elements.value(operationId).value("owner").toString(), invoiceId);
        QCOMPARE(elements.value(operationId).value("name").toString(), QString("pay"));
        bool foundRelation = false, foundView = false;
        for (const auto &value : graph.value("relations").toArray()) {
            const auto record = value.toObject();
            if (record.value("id").toString() == relationId) {
                foundRelation = true;
                QCOMPARE(record.value("kind").toString(), QString("Association"));
                QCOMPARE(record.value("endpoints").toArray(), QJsonArray({invoiceId, customerId}));
            }
        }
        for (const auto &value : graph.value("presentations").toArray()) {
            const auto record = value.toObject();
            if (record.value("id").toString() == viewId) {
                foundView = true;
                QCOMPARE(record.value("element").toString(), invoiceId);
                QCOMPARE(record.value("geometry").toObject().value("x").toDouble(), 100.0);
                QCOMPARE(record.value("geometry").toObject().value("y").toDouble(), 200.0);
            }
        }
        QVERIFY(foundRelation); QVERIFY(foundView);
        reopened.closeWriteChannel(); QVERIFY(reopened.waitForFinished()); QCOMPARE(reopened.exitCode(), 0);
    }
    void insertedClassPreservesLegacyEncodingAndLineEndings() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QVERIFY(QDir().mkpath(dir.filePath("Shared Components")));
        const auto path = dir.filePath("Shared Components/legacy.mdl");
        const QByteArray source = "(object Petal version 50 charSet 0)\r\n"
            "(object Class_Category \"Logical View\" quid \"650000000001\" logical_models (list unit_reference_list "
            "(object Class \"Caf\xe9\" quid \"650000000002\" vendorExtension (\"foreign\" 9))))\r\n";
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(source), source.size()); file.close();
        QProcess process;
        process.start(program(), {"session", "--file", path, "--allow-root", dir.path(), "--source-encoding", "Windows-1252"});
        QVERIFY(process.waitForStarted());
        const auto tree = request(process, {{"id", "tree"}, {"method", "inspect"}, {"params", QJsonObject{{"kind", "modelTree"}}}});
        QVERIFY(tree.value("ok").toBool());
        const auto units = tree.value("result").toObject().value("units").toArray();
        QCOMPARE(units.size(), 1);
        const auto unit = units.first().toObject().value("id").toString();
        QCOMPARE(unit, path);
        QString owner;
        for (const auto &value : tree.value("result").toObject().value("elements").toArray()) if (value.toObject().value("kind") == "Class_Category") owner = value.toObject().value("id").toString();
        QVERIFY(!owner.isEmpty());
        const auto proposal = request(process, {{"id", "new-class"}, {"method", "propose"}, {"params", QJsonObject{{"baseRevision", tree.value("revision")}, {"commands", QJsonArray{QJsonObject{{"type", "createElement"}, {"clientId", "cream"}, {"kind", "Class"}, {"name", QStringLiteral("Crème")}, {"owner", owner}, {"unit", unit}}}}}}});
        QVERIFY2(proposal.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(proposal).toJson())));
        const auto pending = proposal.value("result").toObject();
        const auto newId = pending.value("newIds").toObject().value("cream").toString(); QVERIFY(!newId.isEmpty());
        QVERIFY(request(process, {{"id", "apply"}, {"method", "apply"}, {"params", QJsonObject{{"id", pending.value("id")}, {"baseRevision", pending.value("baseRevision")}, {"digest", pending.value("digest")}}}}).value("ok").toBool());
        QVERIFY(request(process, {{"id", "save"}, {"method", "save"}, {"params", QJsonObject{}}}).value("ok").toBool());
        process.closeWriteChannel(); QVERIFY(process.waitForFinished()); QCOMPARE(process.exitCode(), 0);
        QVERIFY(file.open(QIODevice::ReadOnly)); const auto saved = file.readAll(); file.close();
        QVERIFY(saved.contains("\"Caf\xe9\"")); QVERIFY(saved.contains("\"Cr\xe8me\""));
        QVERIFY(saved.contains("vendorExtension (\"foreign\" 9)"));
        auto withoutCRLF = saved; withoutCRLF.replace("\r\n", "");
        QVERIFY2(!withoutCRLF.contains('\n'), "Insertion introduced a lone LF into a CRLF native document");
        QProcess reopened;
        reopened.start(program(), {"inspect", "--file", path, "--allow-root", dir.path(), "--source-encoding", "Windows-1252"});
        QVERIFY(reopened.waitForFinished()); QCOMPARE(reopened.exitCode(), 0);
        const auto projection = QJsonDocument::fromJson(reopened.readAllStandardOutput()).object();
        bool found = false;
        for (const auto &value : projection.value("elements").toArray()) {
            const auto record = value.toObject();
            if (record.value("id").toString() == newId) { found = true; QCOMPARE(record.value("name").toString(), QStringLiteral("Crème")); QCOMPARE(record.value("owner").toString(), owner); }
        }
        QVERIFY(found);
    }
};
QTEST_GUILESS_MAIN(CliTests)
#include "tst_cli.moc"
