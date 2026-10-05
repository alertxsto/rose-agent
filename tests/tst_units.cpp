#include <QtTest>
#include <QTemporaryDir>
#include <algorithm>
#include "core/ControlledUnits.h"
#include "core/Workspace.h"
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

using namespace rose;
class UnitTests final : public QObject {
    Q_OBJECT
private slots:
    void explicitVariablesSeparatorsAndCycles();
    void missingReferencesRemainVisible();
    void symlinkOutsideGrantIsDenied();
    void referenceModelsAreReadOnly();
    void saveAsIncludesDependencies();
    void cycleKeepsRootWritableAndSharedUnitsUnique();
    void saveAsChecksSourceAndDestination();
    void relocationRewritesCurrentSpansOnly();
    void unknownFileFieldsAreNotLoaded();
    void validRelativeSpellingIsPreserved();
    void saveAsBasenameCollisionKeepsRootWritable();
    void saveAsArbitraryRootSuffix_data();
    void saveAsArbitraryRootSuffix();
    void saveAsCanonicalSymlinkDestination();
    void saveAsArbitrarySuffixKeepsRootCycleResolved();
};
static void put(const QString &path, const QByteArray &bytes) {
    QFile f(path); if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size()) qFatal("fixture write failed");
}
static bool directoryLink(const QString &target, const QString &link) {
#ifdef Q_OS_WIN
    const auto nativeTarget = QDir::toNativeSeparators(target), nativeLink = QDir::toNativeSeparators(link);
    const auto targetName = reinterpret_cast<LPCWSTR>(nativeTarget.utf16()), linkName = reinterpret_cast<LPCWSTR>(nativeLink.utf16());
    // QFile::link creates a shell .lnk on Windows. These tests need a real
    // directory reparse point, including on systems predating Developer Mode.
    if (CreateSymbolicLinkW(linkName, targetName, SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2)) return true;
    DWORD error = GetLastError();
    if (error == ERROR_INVALID_PARAMETER) {
        if (CreateSymbolicLinkW(linkName, targetName, SYMBOLIC_LINK_FLAG_DIRECTORY)) return true;
        error = GetLastError();
    }
    if (error == ERROR_PRIVILEGE_NOT_HELD) {
        QTest::qSkip("OS does not grant native symlink creation privilege", __FILE__, __LINE__); return false;
    }
#else
    if (::symlink(QFile::encodeName(target).constData(), QFile::encodeName(link).constData()) == 0) return true;
    const int error = errno;
    if (error == EPERM || error == EACCES) {
        QTest::qSkip("OS denies native symlink creation privilege", __FILE__, __LINE__); return false;
    }
#endif
    QTest::qFail(qPrintable(QString("Native directory symlink creation failed: %1").arg(error)), __FILE__, __LINE__);
    return false;
}
template<class T> static QString storageError(const Outcome<T> &result) {
    if (const auto error = std::get_if<WorkspaceError>(&result))
        return QString("WorkspaceError %1: %2 [%3]").arg(int(error->code)).arg(error->message, error->file);
    return {};
}
static QByteArray root(const QByteArray &reference) {
    return "(object Petal version 50 charSet 0)\n(object Design \"Logical View\" is_unit TRUE is_loaded TRUE quid \"650000000000\" root_category (object Class_Category \"Logical View\" quid \"650000000001\" logical_models (list unit_reference_list " + reference + ")))\n";
}
static QByteArray ref(const QByteArray &path, const QByteArray &uid = "650000000003") {
    return "(object Class_Category \"Shared\" is_unit TRUE is_loaded FALSE file_name \"" + path + "\" quid \"" + uid + "\")";
}
void UnitTests::explicitVariablesSeparatorsAndCycles() {
    QTemporaryDir dir; QVERIFY(QDir().mkpath(dir.filePath("Shared Components")));
    auto policy = AccessPolicy{{dir.path()}, {{"FRAMEWORK_PATH", dir.path()}}, "ASCII", true};
    auto resolved = ControlledUnits::resolvePath("$FRAMEWORK_PATH\\Shared Components\\shared.cat", dir.filePath("model.mdl"), policy, true);
    QVERIFY(std::holds_alternative<QString>(resolved)); QCOMPARE(std::get<QString>(resolved), dir.filePath("Shared Components/shared.cat"));
    auto absentVariable = ControlledUnits::resolvePath("$UNSET\\x.cat", dir.filePath("model.mdl"), policy, true);
    QVERIFY(std::holds_alternative<WorkspaceError>(absentVariable));
    put(dir.filePath("model.mdl"), root(ref("$FRAMEWORK_PATH\\\\Shared Components\\\\shared.cat")));
    put(dir.filePath("Shared Components/shared.cat"), "(object Petal version 50 charSet 0)\n(object Class_Category \"Shared\" quid \"650000000003\" logical_models (list unit_reference_list " + ref("../model.mdl", "650000000000") + "))\n");
    auto loaded = ControlledUnits::load(dir.filePath("model.mdl"), policy); QVERIFY(std::holds_alternative<LoadedUnits>(loaded));
    QCOMPARE(std::get<LoadedUnits>(loaded).documents.size(), 2);
    bool cycle = false; for (const auto &d : std::get<LoadedUnits>(loaded).diagnostics) if (d.code == "UnitCycle") cycle = true;
    QVERIFY(cycle);
}
void UnitTests::missingReferencesRemainVisible() {
    QTemporaryDir dir; put(dir.filePath("model.mdl"), root(ref("missing.cat")));
    auto loaded = ControlledUnits::load(dir.filePath("model.mdl"), {{dir.path()}, {}, "ASCII", true});
    QVERIFY(std::holds_alternative<LoadedUnits>(loaded)); bool missing = false;
    for (const auto &s : std::get<LoadedUnits>(loaded).summaries) if (!s.resolved) missing = true;
    QVERIFY(missing); QCOMPARE(std::get<LoadedUnits>(loaded).documents.size(), 1);
}
void UnitTests::symlinkOutsideGrantIsDenied() {
    QTemporaryDir allowed, outside; put(outside.filePath("secret.cat"), "(object Class_Category \"Secret\" quid \"650000000003\")");
    const auto link = allowed.filePath("escape");
    if (!directoryLink(outside.path(), link)) return;
    QVERIFY(QFileInfo(link).isDir());
    QCOMPARE(QFileInfo(link).canonicalFilePath(), QFileInfo(outside.path()).canonicalFilePath());
    auto resolved = ControlledUnits::resolvePath("escape/secret.cat", allowed.filePath("model.mdl"), {{allowed.path()}, {}, "ASCII", true});
    QVERIFY(std::holds_alternative<WorkspaceError>(resolved)); QCOMPARE(std::get<WorkspaceError>(resolved).code, ErrorCode::AccessDenied);
    put(allowed.filePath("model.mdl"), root(ref("escape/secret.cat")));
    auto loaded = ControlledUnits::load(allowed.filePath("model.mdl"), {{allowed.path()}, {}, "ASCII", true});
    QVERIFY(std::holds_alternative<LoadedUnits>(loaded)); QCOMPARE(std::get<LoadedUnits>(loaded).documents.size(), 1);
    bool denied = false; for (const auto &d : std::get<LoadedUnits>(loaded).diagnostics) if (d.code == "AccessDenied") denied = true;
    QVERIFY(denied);
    QFile secret(outside.filePath("secret.cat")); QVERIFY(secret.open(QIODevice::ReadOnly));
    QCOMPARE(secret.readAll(), QByteArray("(object Class_Category \"Secret\" quid \"650000000003\")"));
}
void UnitTests::referenceModelsAreReadOnly() {
    QTemporaryDir dir; put(dir.filePath("model.mdl"), root(ref("reference.mdl")));
    put(dir.filePath("reference.mdl"), "(object Petal version 50 charSet 0)\n(object Class_Category \"Shared\" quid \"650000000003\" logical_models (list unit_reference_list (object Class \"Library\" quid \"650000000004\")))\n");
    auto opened = Workspace::open(dir.filePath("model.mdl"), {{dir.path()}, {}, "ASCII", true});
    QVERIFY(std::holds_alternative<std::unique_ptr<Workspace>>(opened)); auto w = std::move(std::get<std::unique_ptr<Workspace>>(opened));
    Query q; q.kind = Query::Kind::Search; q.search = "Library"; auto inspected = w->inspect(q);
    QVERIFY(std::holds_alternative<Projection>(inspected)); const auto &elements = std::get<Projection>(inspected).elements;
    QCOMPARE(elements.size(), 1); QVERIFY(elements.front().readOnly);
    auto proposal = w->propose(w->revision(), {Command{RenameElement{elements.front().id, "Mutated"}}});
    QVERIFY(std::holds_alternative<WorkspaceError>(proposal));
}
void UnitTests::saveAsIncludesDependencies() {
    QTemporaryDir dir; QVERIFY(QDir().mkpath(dir.filePath("copy")));
    put(dir.filePath("model.mdl"), root(ref("shared.cat")));
    put(dir.filePath("shared.cat"), "(object Petal version 50 charSet 0)\n(object Class_Category \"Shared\" quid \"650000000003\" logical_models (list unit_reference_list (object Class \"Library\" quid \"650000000004\")))\n");
    auto opened = Workspace::open(dir.filePath("model.mdl"), {{dir.path()}, {}, "ASCII", true});
    QVERIFY(std::holds_alternative<std::unique_ptr<Workspace>>(opened)); auto w = std::move(std::get<std::unique_ptr<Workspace>>(opened));
    const auto copy = dir.filePath("copy/copy.mdl"); auto saved = w->save({copy});
    QVERIFY2(std::holds_alternative<SaveReceipt>(saved), qPrintable(storageError(saved)));
    QCOMPARE(w->path(), copy);
    auto loaded = ControlledUnits::load(copy, {{dir.path()}, {}, "ASCII", true}); QVERIFY(std::holds_alternative<LoadedUnits>(loaded));
    QCOMPARE(std::get<LoadedUnits>(loaded).documents.size(), 2);
    for (const auto &u : std::get<LoadedUnits>(loaded).documents) QVERIFY(u.path.startsWith(dir.filePath("copy") + "/"));
    QFile original(dir.filePath("shared.cat")); QVERIFY(original.open(QIODevice::ReadOnly));
    QVERIFY(original.readAll().contains("Library"));
}
void UnitTests::cycleKeepsRootWritableAndSharedUnitsUnique() {
    QTemporaryDir dir;
    put(dir.filePath("model.mdl"), root(ref("shared.cat") + ref("shared.cat")));
    put(dir.filePath("shared.cat"), "(object Class_Category \"Shared\" quid \"650000000003\" logical_models (list unit_reference_list " + ref("model.mdl", "650000000000") + "))");
    auto loaded = ControlledUnits::load(dir.filePath("model.mdl"), {{dir.path()}});
    QVERIFY(std::holds_alternative<LoadedUnits>(loaded));
    const auto &units = std::get<LoadedUnits>(loaded);
    QCOMPARE(units.documents.size(), 2);
    for (const auto &document : units.documents) {
        QCOMPARE(document.id.value, document.path);
        QVERIFY(document.writable);
    }
}
void UnitTests::saveAsChecksSourceAndDestination() {
    QTemporaryDir dir; QVERIFY(QDir().mkpath(dir.filePath("copy")));
    const auto source = dir.filePath("model.mdl"), destination = dir.filePath("copy/model.mdl");
    put(source, root(ref("shared.cat")));
    put(dir.filePath("shared.cat"), "(object Class_Category \"Shared\" quid \"650000000003\")");
    const AccessPolicy policy{{dir.path()}};
    auto result = ControlledUnits::load(source, policy); QVERIFY(std::holds_alternative<LoadedUnits>(result));
    const auto &units = std::get<LoadedUnits>(result);
    QMap<UnitId, QByteArray> bytes;
    for (const auto &document : units.documents) bytes.insert(document.id, document.document.bytes());
    put(destination, "existing");
    auto collision = ControlledUnits::relocate(units, bytes, source, destination, policy);
    QVERIFY(std::holds_alternative<WorkspaceError>(collision));
    QCOMPARE(std::get<WorkspaceError>(collision).code, ErrorCode::DiskConflict);
    QVERIFY(QFile::remove(destination));
    auto internalCollision = ControlledUnits::relocate(units, bytes, source, dir.filePath("copy/shared.cat"), policy);
    QVERIFY(std::holds_alternative<WorkspaceError>(internalCollision));
    QCOMPARE(std::get<WorkspaceError>(internalCollision).code, ErrorCode::UnsafeRewrite);
    put(dir.filePath("shared.cat"), "(object Class_Category \"Externally Changed\" quid \"650000000003\")");
    auto changed = ControlledUnits::relocate(units, bytes, source, destination, policy);
    QVERIFY(std::holds_alternative<WorkspaceError>(changed));
    QCOMPARE(std::get<WorkspaceError>(changed).code, ErrorCode::DiskConflict);
    QVERIFY(!QFileInfo::exists(destination));
}
void UnitTests::relocationRewritesCurrentSpansOnly() {
    QTemporaryDir dir; QVERIFY(QDir().mkpath(dir.filePath("copy")));
    const auto source = dir.filePath("model.mdl");
    put(source, root(ref("$UNITS/shared.cat")));
    put(dir.filePath("shared.cat"), "(object Class_Category \"Shared\" quid \"650000000003\")");
    const AccessPolicy policy{{dir.path()}, {{"UNITS", dir.path()}}};
    auto result = ControlledUnits::load(source, policy); QVERIFY(std::holds_alternative<LoadedUnits>(result));
    const auto &units = std::get<LoadedUnits>(result);
    QMap<UnitId, QByteArray> bytes;
    for (const auto &document : units.documents) bytes.insert(document.id, document.document.bytes());
    bytes[UnitId{source}].replace("is_unit TRUE is_loaded TRUE", "vendorMarker \"unchanged\" is_unit TRUE is_loaded TRUE");
    const auto destination = dir.filePath("copy/model.mdl");
    auto relocated = ControlledUnits::relocate(units, bytes, source, destination, policy);
    QVERIFY(std::holds_alternative<QVector<storage::Write>>(relocated));
    const auto &writes = std::get<QVector<storage::Write>>(relocated);
    QCOMPARE(writes.size(), 2);
    QByteArray expected = bytes.value(UnitId{source});
    expected.replace("file_name \"$UNITS/shared.cat\"", "file_name \"shared.cat\"");
    for (const auto &write : writes) {
        QVERIFY(!write.expectedChecksum.has_value());
        if (write.path == destination) QCOMPARE(write.bytes, expected);
    }
    auto saved = storage::save(writes, destination, policy);
    QVERIFY2((std::holds_alternative<QMap<QString, QByteArray>>(saved)), qPrintable(storageError(saved)));
    auto reopened = ControlledUnits::load(destination, policy); QVERIFY(std::holds_alternative<LoadedUnits>(reopened));
    QCOMPARE(std::get<LoadedUnits>(reopened).documents.size(), 2);
    QFile original(source); QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), root(ref("$UNITS/shared.cat")));
}
void UnitTests::unknownFileFieldsAreNotLoaded() {
    QTemporaryDir dir;
    const auto source = dir.filePath("model.mdl");
    const QByteArray bytes = root("(object VendorResource \"Unknown\" file_name \"do-not-read.cat\")");
    put(source, bytes);
    put(dir.filePath("do-not-read.cat"), "not a native document");
    auto loaded = ControlledUnits::load(source, {{dir.path()}}); QVERIFY(std::holds_alternative<LoadedUnits>(loaded));
    const auto &units = std::get<LoadedUnits>(loaded);
    QCOMPARE(units.documents.size(), 1);
    QCOMPARE(units.documents.front().document.bytes(), bytes);
    auto relocated = ControlledUnits::relocate(units, {{UnitId{source}, bytes}}, source, dir.filePath("copy.mdl"), {{dir.path()}});
    QVERIFY(std::holds_alternative<WorkspaceError>(relocated));
    QCOMPARE(std::get<WorkspaceError>(relocated).code, ErrorCode::UnsafeRewrite);
}
void UnitTests::validRelativeSpellingIsPreserved() {
    QTemporaryDir dir; QVERIFY(QDir().mkpath(dir.filePath("copy")));
    const auto source = dir.filePath("model.mdl"), destination = dir.filePath("copy/model.mdl");
    const QByteArray original = root(ref(".\\\\shared.cat"));
    put(source, original);
    put(dir.filePath("shared.cat"), "(object Class_Category \"Shared\" quid \"650000000003\")");
    const AccessPolicy policy{{dir.path()}};
    auto loaded = ControlledUnits::load(source, policy); QVERIFY(std::holds_alternative<LoadedUnits>(loaded));
    const auto &units = std::get<LoadedUnits>(loaded);
    QMap<UnitId, QByteArray> bytes;
    for (const auto &document : units.documents) bytes.insert(document.id, document.document.bytes());
    auto relocated = ControlledUnits::relocate(units, bytes, source, destination, policy);
    QVERIFY(std::holds_alternative<QVector<storage::Write>>(relocated));
    const auto &writes = std::get<QVector<storage::Write>>(relocated);
    auto rootWrite = std::find_if(writes.cbegin(), writes.cend(), [&](const auto &write) { return write.path == destination; });
    QVERIFY(rootWrite != writes.cend()); QCOMPARE(rootWrite->bytes, original);
}
void UnitTests::saveAsBasenameCollisionKeepsRootWritable() {
    QTemporaryDir dir;
    QVERIFY(QDir().mkpath(dir.filePath("nested")));
    QVERIFY(QDir().mkpath(dir.filePath("copy/nested")));
    put(dir.filePath("model.mdl"), root(ref("nested/ref.mdl")));
    put(dir.filePath("nested/ref.mdl"), "(object Petal version 50 charSet 0)\n(object Class_Category \"Shared\" quid \"650000000003\" logical_models (list unit_reference_list (object Class \"Library\" quid \"650000000004\") " + ref("child.cat", "650000000005") + "))\n");
    put(dir.filePath("nested/child.cat"), "(object Petal version 50 charSet 0)\n(object Class_Category \"Child\" quid \"650000000005\" logical_models (list unit_reference_list (object Class \"Inherited\" quid \"650000000006\")))\n");
    auto opened = Workspace::open(dir.filePath("model.mdl"), {{dir.path()}});
    QVERIFY(std::holds_alternative<std::unique_ptr<Workspace>>(opened));
    auto workspace = std::move(std::get<std::unique_ptr<Workspace>>(opened));
    auto saved = workspace->save({dir.filePath("copy/ref.mdl")});
    QVERIFY2(std::holds_alternative<SaveReceipt>(saved), qPrintable(storageError(saved)));
    Query rootQuery; rootQuery.kind = Query::Kind::ElementsById; rootQuery.elements = {ElementId{"650000000001"}};
    auto rootProjection = workspace->inspect(rootQuery); QVERIFY(std::holds_alternative<Projection>(rootProjection));
    const auto &rootElements = std::get<Projection>(rootProjection).elements;
    QCOMPARE(rootElements.size(), 1); QVERIFY(!rootElements.front().readOnly);
    auto proposal = workspace->propose(workspace->revision(), {Command{RenameElement{rootElements.front().id, "Writable Root"}}});
    QVERIFY(std::holds_alternative<Proposal>(proposal));
    const auto &approved = std::get<Proposal>(proposal);
    auto applied = workspace->apply({approved.id, approved.baseRevision, approved.digest});
    QVERIFY(std::holds_alternative<Applied>(applied));
    rootProjection = workspace->inspect(rootQuery); QVERIFY(std::holds_alternative<Projection>(rootProjection));
    QCOMPARE(std::get<Projection>(rootProjection).elements.front().name, QString("Writable Root"));
    Query libraryQuery; libraryQuery.kind = Query::Kind::Search; libraryQuery.search = "Library";
    auto libraryProjection = workspace->inspect(libraryQuery); QVERIFY(std::holds_alternative<Projection>(libraryProjection));
    const auto &library = std::get<Projection>(libraryProjection).elements;
    QCOMPARE(library.size(), 1); QVERIFY(library.front().readOnly);
    QCOMPARE(library.front().unit.value, dir.filePath("copy/nested/ref.mdl"));
    auto denied = workspace->propose(workspace->revision(), {Command{RenameElement{library.front().id, "Mutated"}}});
    QVERIFY(std::holds_alternative<WorkspaceError>(denied));
    Query childQuery; childQuery.kind = Query::Kind::ElementsById; childQuery.elements = {ElementId{"650000000006"}};
    auto childProjection = workspace->inspect(childQuery); QVERIFY(std::holds_alternative<Projection>(childProjection));
    const auto &child = std::get<Projection>(childProjection).elements;
    QCOMPARE(child.size(), 1); QVERIFY(child.front().readOnly);
    QCOMPARE(child.front().unit.value, dir.filePath("copy/nested/child.cat"));
    auto childDenied = workspace->propose(workspace->revision(), {Command{RenameElement{child.front().id, "Mutated Child"}}});
    QVERIFY(std::holds_alternative<WorkspaceError>(childDenied));
}
void UnitTests::saveAsArbitraryRootSuffix_data() {
    QTest::addColumn<bool>("withDependency");
    QTest::newRow("created-root") << false;
    QTest::newRow("root-with-controlled-child") << true;
}
void UnitTests::saveAsArbitraryRootSuffix() {
    QFETCH(bool, withDependency);
    QTemporaryDir dir; QVERIFY(QDir().mkpath(dir.filePath("copy")));
    const auto source = dir.filePath("source.mdl"), destination = dir.filePath("copy/copy.native");
    const AccessPolicy policy{{dir.path()}};
    if (withDependency) {
        put(source, root(ref("shared.cat")));
        put(dir.filePath("shared.cat"), "(object Class_Category \"Shared\" quid \"650000000003\" logical_models (list unit_reference_list (object Class \"Library\" quid \"650000000004\")))\n");
    }
    auto opened = withDependency ? Workspace::open(source, policy) : Workspace::create(source, {}, policy);
    QVERIFY(std::holds_alternative<std::unique_ptr<Workspace>>(opened));
    auto workspace = std::move(std::get<std::unique_ptr<Workspace>>(opened));
    auto initialSaved = workspace->save();
    QVERIFY2(std::holds_alternative<SaveReceipt>(initialSaved), qPrintable(storageError(initialSaved)));
    auto before = workspace->inspect({});
    QVERIFY(std::holds_alternative<Projection>(before));
    const auto &original = std::get<Projection>(before).elements;
    const auto category = std::find_if(original.cbegin(), original.cend(), [](const auto &element) {
        return element.kind == "Class_Category" && element.name == "Logical View";
    });
    QVERIFY(category != original.cend());
    Query query; query.kind = Query::Kind::ElementsById;
    for (const auto &element : original) query.elements.append(element.id);
    auto saved = workspace->save({destination});
    QVERIFY2(std::holds_alternative<SaveReceipt>(saved), qPrintable(storageError(saved)));
    QCOMPARE(workspace->path(), destination);
    auto relocated = workspace->inspect(query);
    QVERIFY(std::holds_alternative<Projection>(relocated));
    const auto &elements = std::get<Projection>(relocated).elements;
    QCOMPARE(elements.size(), original.size());
    for (qsizetype i = 0; i < original.size(); ++i) {
        QCOMPARE(elements[i].id.value, original[i].id.value);
        QCOMPARE(elements[i].kind, original[i].kind);
        QCOMPARE(elements[i].name, original[i].name);
        QCOMPARE(elements[i].owner.value, original[i].owner.value);
        QCOMPARE(elements[i].readOnly, original[i].readOnly);
        QCOMPARE(elements[i].unit.value, original[i].unit.value == source ? destination : dir.filePath("copy/shared.cat"));
    }
    auto proposal = workspace->propose(workspace->revision(), {Command{RenameElement{category->id, "Renamed Root"}}});
    QVERIFY(std::holds_alternative<Proposal>(proposal));
    const auto &approved = std::get<Proposal>(proposal);
    QVERIFY(std::holds_alternative<Applied>(workspace->apply({approved.id, approved.baseRevision, approved.digest})));
    auto editedSaved = workspace->save();
    QVERIFY2(std::holds_alternative<SaveReceipt>(editedSaved), qPrintable(storageError(editedSaved)));
    auto reopened = Workspace::open(destination, policy);
    QVERIFY(std::holds_alternative<std::unique_ptr<Workspace>>(reopened));
    Query rootQuery; rootQuery.kind = Query::Kind::ElementsById; rootQuery.elements = {category->id};
    auto rootProjection = std::get<std::unique_ptr<Workspace>>(reopened)->inspect(rootQuery);
    QVERIFY(std::holds_alternative<Projection>(rootProjection));
    const auto &rootElements = std::get<Projection>(rootProjection).elements;
    QCOMPARE(rootElements.size(), 1);
    QCOMPARE(rootElements.front().name, QString("Renamed Root"));
    QCOMPARE(rootElements.front().unit.value, destination);
    QVERIFY(!rootElements.front().readOnly);
    if (withDependency) {
        Query childQuery; childQuery.kind = Query::Kind::ElementsById; childQuery.elements = {ElementId{"650000000004"}};
        auto childProjection = std::get<std::unique_ptr<Workspace>>(reopened)->inspect(childQuery);
        QVERIFY(std::holds_alternative<Projection>(childProjection));
        const auto &children = std::get<Projection>(childProjection).elements;
        QCOMPARE(children.size(), 1);
        QCOMPARE(children.front().name, QString("Library"));
        QCOMPARE(children.front().owner.value, QString("650000000003"));
        QCOMPARE(children.front().unit.value, dir.filePath("copy/shared.cat"));
        QVERIFY(!children.front().readOnly);
    }
}
void UnitTests::saveAsCanonicalSymlinkDestination() {
    QTemporaryDir dir;
    QVERIFY(QDir().mkpath(dir.filePath("nested")));
    QVERIFY(QDir().mkpath(dir.filePath("copy/units")));
    const auto link = dir.filePath("copy/nested");
    if (!directoryLink(dir.filePath("copy/units"), link)) return;
    QVERIFY(QFileInfo(link).isDir());
    QCOMPARE(QFileInfo(link).canonicalFilePath(), QFileInfo(dir.filePath("copy/units")).canonicalFilePath());
    const auto source = dir.filePath("model.mdl"), destination = dir.filePath("copy/model.mdl");
    const auto shared = dir.filePath("copy/units/shared.cat"), reference = dir.filePath("copy/units/ref.mdl");
    put(source, root(ref("nested/shared.cat") + ref("nested/ref.mdl", "650000000005")));
    const QByteArray sharedBytes = "(object Class_Category \"Shared\" quid \"650000000003\" logical_models (list unit_reference_list (object Class \"Library\" quid \"650000000004\")))\n";
    const QByteArray referenceBytes = "(object Petal version 50 charSet 0)\n(object Class_Category \"Reference\" quid \"650000000005\" logical_models (list unit_reference_list (object Class \"Read Only\" quid \"650000000006\")))\n";
    put(dir.filePath("nested/shared.cat"), sharedBytes);
    put(dir.filePath("nested/ref.mdl"), referenceBytes);
    const AccessPolicy policy{{dir.path()}};
    auto opened = Workspace::open(source, policy);
    QVERIFY(std::holds_alternative<std::unique_ptr<Workspace>>(opened));
    auto workspace = std::move(std::get<std::unique_ptr<Workspace>>(opened));
    auto saved = workspace->save({destination});
    QVERIFY2(std::holds_alternative<SaveReceipt>(saved), qPrintable(storageError(saved)));
    Query query; query.kind = Query::Kind::ElementsById;
    query.elements = {ElementId{"650000000001"}, ElementId{"650000000004"}, ElementId{"650000000006"}};
    auto projection = workspace->inspect(query);
    QVERIFY(std::holds_alternative<Projection>(projection));
    const auto &elements = std::get<Projection>(projection).elements;
    QCOMPARE(elements.size(), 3);
    QCOMPARE(elements[0].unit.value, destination); QVERIFY(!elements[0].readOnly);
    QCOMPARE(elements[1].unit.value, shared); QVERIFY(!elements[1].readOnly);
    QCOMPARE(elements[1].owner.value, QString("650000000003"));
    QCOMPARE(elements[2].unit.value, reference); QVERIFY(elements[2].readOnly);
    QCOMPARE(elements[2].owner.value, QString("650000000005"));
    const auto &summaries = std::get<Projection>(projection).units;
    QCOMPARE(summaries.size(), 3);
    for (const auto &unit : summaries) {
        QVERIFY(unit.resolved);
        QCOMPARE(unit.id.value, unit.path);
        QVERIFY(unit.path == destination || unit.path == shared || unit.path == reference);
        QCOMPARE(unit.writable, unit.path != reference);
    }
    auto denied = workspace->propose(workspace->revision(), {Command{RenameElement{elements[2].id, "Forbidden"}}});
    QVERIFY(std::holds_alternative<WorkspaceError>(denied));
    QCOMPARE(std::get<WorkspaceError>(denied).code, ErrorCode::AccessDenied);
    auto proposal = workspace->propose(workspace->revision(), {Command{RenameElement{elements[1].id, "Writable Library"}}});
    QVERIFY(std::holds_alternative<Proposal>(proposal));
    const auto &approved = std::get<Proposal>(proposal);
    QVERIFY(std::holds_alternative<Applied>(workspace->apply({approved.id, approved.baseRevision, approved.digest})));
    auto editedSaved = workspace->save();
    QVERIFY2(std::holds_alternative<SaveReceipt>(editedSaved), qPrintable(storageError(editedSaved)));
    auto reopened = Workspace::open(destination, policy);
    QVERIFY(std::holds_alternative<std::unique_ptr<Workspace>>(reopened));
    auto reopenedProjection = std::get<std::unique_ptr<Workspace>>(reopened)->inspect(query);
    QVERIFY(std::holds_alternative<Projection>(reopenedProjection));
    const auto &reopenedElements = std::get<Projection>(reopenedProjection).elements;
    QCOMPARE(reopenedElements.size(), 3);
    QCOMPARE(reopenedElements[1].name, QString("Writable Library"));
    QCOMPARE(reopenedElements[1].unit.value, shared); QVERIFY(!reopenedElements[1].readOnly);
    QCOMPARE(reopenedElements[2].name, QString("Read Only"));
    QCOMPARE(reopenedElements[2].unit.value, reference); QVERIFY(reopenedElements[2].readOnly);
    QFile original(dir.filePath("nested/shared.cat")); QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), sharedBytes);
    QFile copiedReference(reference); QVERIFY(copiedReference.open(QIODevice::ReadOnly));
    QCOMPARE(copiedReference.readAll(), referenceBytes);
}
void UnitTests::saveAsArbitrarySuffixKeepsRootCycleResolved() {
    QTemporaryDir dir; QVERIFY(QDir().mkpath(dir.filePath("copy")));
    const auto source = dir.filePath("model.mdl"), destination = dir.filePath("copy/copy.native");
    const auto rootBytes = root(ref("shared.cat"));
    const QByteArray sharedBytes = "(object Class_Category \"Shared\" quid \"650000000003\" logical_models"
        " (list unit_reference_list (object Design \"Logical View\" quid \"650000000000\""
        " is_unit TRUE is_loaded FALSE file_name \"model.mdl\")))\n";
    put(source, rootBytes); put(dir.filePath("shared.cat"), sharedBytes);
    const AccessPolicy policy{{dir.path()}};
    auto opened = Workspace::open(source, policy); QVERIFY(std::holds_alternative<std::unique_ptr<Workspace>>(opened));
    auto workspace = std::move(std::get<std::unique_ptr<Workspace>>(opened));
    auto saved = workspace->save({destination});
    QVERIFY2(std::holds_alternative<SaveReceipt>(saved), qPrintable(storageError(saved)));
    auto loaded = ControlledUnits::load(destination, policy); QVERIFY(std::holds_alternative<LoadedUnits>(loaded));
    const auto &units = std::get<LoadedUnits>(loaded);
    QCOMPARE(units.documents.size(), 2);
    for (const auto &reference : units.references) QVERIFY(reference.resolved);
    auto proposed = workspace->propose(workspace->revision(),
        {Command{CreateElement{"added", "Class", "Added", ElementId{"650000000001"}, {}}}});
    QVERIFY(std::holds_alternative<Proposal>(proposed));
    const auto &proposal = std::get<Proposal>(proposed);
    QVERIFY(std::holds_alternative<Applied>(workspace->apply({proposal.id, proposal.baseRevision, proposal.digest})));
    auto editedSaved = workspace->save();
    QVERIFY2(std::holds_alternative<SaveReceipt>(editedSaved), qPrintable(storageError(editedSaved)));
    auto reopened = Workspace::open(destination, policy); QVERIFY(std::holds_alternative<std::unique_ptr<Workspace>>(reopened));
    Query query; query.kind = Query::Kind::ElementsById; query.elements = {ElementId{proposal.newIds.value("added")}};
    auto projection = std::get<std::unique_ptr<Workspace>>(reopened)->inspect(query);
    QVERIFY(std::holds_alternative<Projection>(projection));
    const auto &elements = std::get<Projection>(projection).elements;
    QCOMPARE(elements.size(), 1); QCOMPARE(elements.front().name, QString("Added"));
    QCOMPARE(elements.front().owner, ElementId{"650000000001"});
    QFile original(source); QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), rootBytes);
    QFile shared(dir.filePath("shared.cat")); QVERIFY(shared.open(QIODevice::ReadOnly)); QCOMPARE(shared.readAll(), sharedBytes);
}

QTEST_GUILESS_MAIN(UnitTests)
#include "tst_units.moc"
