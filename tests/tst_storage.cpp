#include <QtTest>
#include <QTemporaryDir>
#include "core/Storage.h"
#include "core/Workspace.h"
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

using namespace rose;
class StorageTests final : public QObject {
    Q_OBJECT
private slots:
    void externalChangeNeverOverwritten();
    void absentDestinationCannotOverwrite();
    void deniedSymlinkWriteLeavesOutsideUntouched();
    void journalFailureLeavesAllTargetsUntouched();
    void successfulMultiFileSaveReturnsChecksums();
    void durableWritesPreserveExactBytes_data();
    void durableWritesPreserveExactBytes();
    void workspaceConflictRetainsDirtyState();
};
static void put(const QString &p, const QByteArray &b) { QFile f(p); if (!f.open(QIODevice::WriteOnly) || f.write(b) != b.size()) qFatal("fixture write failed"); }
static QByteArray get(const QString &p) { QFile f(p); if (!f.open(QIODevice::ReadOnly)) qFatal("fixture read failed"); return f.readAll(); }
static bool directoryLink(const QString &target, const QString &link) {
#ifdef Q_OS_WIN
    const auto nativeTarget = QDir::toNativeSeparators(target), nativeLink = QDir::toNativeSeparators(link);
    const auto targetName = reinterpret_cast<LPCWSTR>(nativeTarget.utf16()), linkName = reinterpret_cast<LPCWSTR>(nativeLink.utf16());
    // Request unprivileged creation where supported, then use the older API
    // flags on Windows 8.1. QFile::link creates a shell .lnk, not a symlink.
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
    const auto link = dir.filePath("escape");
    if (!directoryLink(outside.path(), link)) return;
    QVERIFY(QFileInfo(link).isDir());
    QCOMPARE(QFileInfo(link).canonicalFilePath(), QFileInfo(outside.path()).canonicalFilePath());
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
    QVERIFY2((std::holds_alternative<QMap<QString, QByteArray>>(result)), qPrintable(storageError(result)));
    QCOMPARE(get(a), QByteArray("a-new")); QCOMPARE(get(b), QByteArray("b-new"));
    QCOMPARE((std::get<QMap<QString, QByteArray>>(result).value(a)), storage::checksum("a-new"));
    QVERIFY(!QFileInfo::exists(storage::journalPath(a)));
}
void StorageTests::durableWritesPreserveExactBytes_data() {
    QTest::addColumn<int>("size");
    QTest::newRow("empty") << 0;
    QTest::newRow("one-byte") << 1;
    QTest::newRow("read-block") << 65536;
    QTest::newRow("multiple-read-blocks") << 65537;
}
void StorageTests::durableWritesPreserveExactBytes() {
    QFETCH(int, size);
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const auto existing = dir.filePath("existing.mdl"), created = dir.filePath("created.cat");
    QByteArray bytes(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i) bytes[i] = char(i % 256);
    const QByteArray original("original");
    put(existing, original);
    auto result = storage::save({{existing, bytes, storage::checksum(original)}, {created, bytes, std::nullopt}},
        existing, {{dir.path()}});
    QVERIFY2((std::holds_alternative<QMap<QString, QByteArray>>(result)), qPrintable(storageError(result)));
    QCOMPARE(get(existing), bytes); QCOMPARE(get(created), bytes);
    const auto &checksums = std::get<QMap<QString, QByteArray>>(result);
    QCOMPARE(checksums.value(existing), storage::checksum(bytes));
    QCOMPARE(checksums.value(created), storage::checksum(bytes));
    QVERIFY(!QFileInfo::exists(storage::journalPath(existing)));
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
