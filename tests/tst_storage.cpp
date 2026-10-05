#include <QtTest>
#include <QTemporaryDir>
#include "core/Storage.h"
#include "core/Workspace.h"

using namespace rose;
class StorageTests final : public QObject {
    Q_OBJECT
private slots:
    void externalChangeNeverOverwritten();
    void absentDestinationCannotOverwrite();
    void deniedSymlinkWriteLeavesOutsideUntouched();
    void journalFailureLeavesAllTargetsUntouched();
    void successfulMultiFileSaveReturnsChecksums();
    void workspaceConflictRetainsDirtyState();
};
static void put(const QString &p, const QByteArray &b) { QFile f(p); if (!f.open(QIODevice::WriteOnly) || f.write(b) != b.size()) qFatal("fixture write failed"); }
static QByteArray get(const QString &p) { QFile f(p); if (!f.open(QIODevice::ReadOnly)) qFatal("fixture read failed"); return f.readAll(); }
void StorageTests::externalChangeNeverOverwritten() {
    QTemporaryDir dir; auto p = dir.filePath("a.mdl"); put(p, "original"); auto policy = AccessPolicy{{dir.path()}};
    auto s = storage::read(p, policy); QVERIFY(std::holds_alternative<storage::Snapshot>(s)); put(p, "external");
    auto result = storage::save({{p, "edited", std::get<storage::Snapshot>(s).checksum}}, p, policy);
    QVERIFY(std::holds_alternative<WorkspaceError>(result)); QCOMPARE(std::get<WorkspaceError>(result).code, ErrorCode::DiskConflict);
    QCOMPARE(get(p), QByteArray("external"));
}
void StorageTests::absentDestinationCannotOverwrite() {
    QTemporaryDir dir; auto p = dir.filePath("a.mdl"); put(p, "external");
    auto result = storage::save({{p, "new", std::nullopt}}, p, {{dir.path()}});
    QVERIFY(std::holds_alternative<WorkspaceError>(result)); QCOMPARE(std::get<WorkspaceError>(result).code, ErrorCode::DiskConflict);
    QCOMPARE(get(p), QByteArray("external"));
}
void StorageTests::deniedSymlinkWriteLeavesOutsideUntouched() {
    QTemporaryDir dir, outside; auto secret = outside.filePath("a.mdl"); put(secret, "external");
    if (!QFile::link(outside.path(), dir.filePath("escape"))) QSKIP("directory links unavailable");
    auto p = dir.filePath("escape/a.mdl");
    auto result = storage::save({{p, "edited", storage::checksum("external")}}, p, {{dir.path()}});
    QVERIFY(std::holds_alternative<WorkspaceError>(result)); QCOMPARE(std::get<WorkspaceError>(result).code, ErrorCode::AccessDenied);
    QCOMPARE(get(secret), QByteArray("external"));
}
void StorageTests::journalFailureLeavesAllTargetsUntouched() {
    QTemporaryDir dir; auto a = dir.filePath("a.mdl"), b = dir.filePath("b.cat"); put(a, "a-old"); put(b, "b-old");
    QVERIFY(QDir().mkdir(storage::journalPath(a)));
    auto result = storage::save({{a, "a-new", storage::checksum("a-old")}, {b, "b-new", storage::checksum("b-old")}}, a, {{dir.path()}});
    QVERIFY(std::holds_alternative<WorkspaceError>(result)); QCOMPARE(get(a), QByteArray("a-old")); QCOMPARE(get(b), QByteArray("b-old"));
}
void StorageTests::successfulMultiFileSaveReturnsChecksums() {
    QTemporaryDir dir; auto a = dir.filePath("a.mdl"), b = dir.filePath("b.cat"); put(a, "a-old"); put(b, "b-old");
    auto result = storage::save({{a, "a-new", storage::checksum("a-old")}, {b, "b-new", storage::checksum("b-old")}}, a, {{dir.path()}});
    QVERIFY((std::holds_alternative<QMap<QString, QByteArray>>(result)));
    QCOMPARE(get(a), QByteArray("a-new")); QCOMPARE(get(b), QByteArray("b-new"));
    QCOMPARE((std::get<QMap<QString, QByteArray>>(result).value(a)), storage::checksum("a-new"));
    QVERIFY(!QFileInfo::exists(storage::journalPath(a)));
}
void StorageTests::workspaceConflictRetainsDirtyState() {
    QTemporaryDir dir; auto path = dir.filePath("a.mdl");
    put(path, "(object Petal version 50 charSet 0)\n(object Class \"Order\" quid \"650000000001\" vendorExtension 9)\n");
    auto opened = Workspace::open(path, {{dir.path()}}); QVERIFY(std::holds_alternative<std::unique_ptr<Workspace>>(opened));
    auto w = std::move(std::get<std::unique_ptr<Workspace>>(opened)); auto viewed = w->inspect({}); QVERIFY(std::holds_alternative<Projection>(viewed));
    const auto id = std::get<Projection>(viewed).elements.front().id;
    auto proposed = w->propose(0, {Command{RenameElement{id, "Invoice"}}}); QVERIFY(std::holds_alternative<Proposal>(proposed));
    const auto p = std::get<Proposal>(proposed); QVERIFY(std::holds_alternative<Applied>(w->apply({p.id, p.baseRevision, p.digest})));
    put(path, "external"); auto saved = w->save(); QVERIFY(std::holds_alternative<WorkspaceError>(saved));
    QCOMPARE(std::get<WorkspaceError>(saved).code, ErrorCode::DiskConflict); QVERIFY(w->dirty()); QCOMPARE(w->revision(), Revision(1));
    QCOMPARE(get(path), QByteArray("external"));
}
QTEST_GUILESS_MAIN(StorageTests)
#include "tst_storage.moc"
