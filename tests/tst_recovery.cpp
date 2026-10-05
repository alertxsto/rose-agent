#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonArray>
#include "core/Storage.h"

using namespace rose;
class RecoveryTests final : public QObject {
    Q_OBJECT
private slots:
    void interruptedReplacementRestoresOriginalSet();
    void externalEditBlocksRecoveryWithoutPartialRollback();
    void tamperedJournalIsPreservedAndRejected();
    void committedJournalPreservesNewSet();
};
static void put(const QString &p, const QByteArray &b) { QFile f(p); if (!f.open(QIODevice::WriteOnly) || f.write(b) != b.size()) qFatal("fixture write failed"); }
static QByteArray get(const QString &p) { QFile f(p); if (!f.open(QIODevice::ReadOnly)) qFatal("fixture read failed"); return f.readAll(); }
static QString storageError(const Status &result) {
    if (const auto error = std::get_if<WorkspaceError>(&result))
        return QString("WorkspaceError %1: %2 [%3]").arg(int(error->code)).arg(error->message, error->file);
    return {};
}
struct RecoveryFixture {
    QString a, b, aBackup, bBackup, aStage, bStage, journal;
    QJsonObject payload;
};
static RecoveryFixture interrupted(const QTemporaryDir &dir, bool committed = false) {
    const QString transaction = "12345678-1234-1234-1234-123456789abc";
    RecoveryFixture f{dir.filePath("a.mdl"), dir.filePath("b.cat"),
        dir.filePath(".rose-txn-" + transaction + "-0.backup"), dir.filePath(".rose-txn-" + transaction + "-1.backup"),
        dir.filePath(".rose-txn-" + transaction + "-0.stage"), dir.filePath(".rose-txn-" + transaction + "-1.stage"), {}, {}};
    f.journal = storage::journalPath(f.a);
    put(f.a, "a-new"); put(f.b, committed ? "b-new" : "b-old"); put(f.aBackup, "a-old"); put(f.bBackup, "b-old");
    put(f.bStage, "b-new");
    auto entry = [](const QString &target, const QString &backup, const QString &stage, QByteArray old, QByteArray next, const QString &state) {
        return QJsonObject{{"target", target}, {"backup", backup}, {"stage", stage}, {"existed", true},
            {"oldChecksum", QString::fromLatin1(storage::checksum(old).toHex())},
            {"newChecksum", QString::fromLatin1(storage::checksum(next).toHex())}, {"state", state}};
    };
    f.payload = {{"schema", "rose-storage-journal/1"}, {"transaction", transaction}, {"modelPath", f.a}, {"committed", committed},
        {"entries", QJsonArray{entry(f.a, f.aBackup, f.aStage, "a-old", "a-new", "replaced"),
                                entry(f.b, f.bBackup, f.bStage, "b-old", "b-new", committed ? "replaced" : "pending")}}};
    auto bytes = QJsonDocument(f.payload).toJson(QJsonDocument::Compact);
    put(f.journal, QJsonDocument(QJsonObject{{"payload", f.payload}, {"checksum", QString::fromLatin1(storage::checksum(bytes).toHex())}}).toJson(QJsonDocument::Compact));
    return f;
}
void RecoveryTests::interruptedReplacementRestoresOriginalSet() {
    QTemporaryDir dir; auto f = interrupted(dir);
    auto result = storage::recover(f.a, {{dir.path()}});
    QVERIFY2(std::holds_alternative<std::monostate>(result), qPrintable(storageError(result)));
    QCOMPARE(get(f.a), QByteArray("a-old")); QCOMPARE(get(f.b), QByteArray("b-old")); QVERIFY(!QFileInfo::exists(f.journal));
    QVERIFY(!QFileInfo::exists(f.aBackup)); QVERIFY(!QFileInfo::exists(f.bStage));
}
void RecoveryTests::externalEditBlocksRecoveryWithoutPartialRollback() {
    QTemporaryDir dir; auto f = interrupted(dir); put(f.b, "external");
    auto result = storage::recover(f.a, {{dir.path()}}); QVERIFY(std::holds_alternative<WorkspaceError>(result));
    QCOMPARE(std::get<WorkspaceError>(result).code, ErrorCode::DiskConflict);
    QCOMPARE(get(f.a), QByteArray("a-new")); QCOMPARE(get(f.b), QByteArray("external")); QVERIFY(QFileInfo::exists(f.journal));
    QVERIFY(QFileInfo::exists(f.aBackup));
}
void RecoveryTests::tamperedJournalIsPreservedAndRejected() {
    QTemporaryDir dir; auto f = interrupted(dir); put(f.journal, "{\"payload\":{},\"checksum\":\"bad\"}");
    auto result = storage::recover(f.a, {{dir.path()}}); QVERIFY(std::holds_alternative<WorkspaceError>(result));
    QCOMPARE(std::get<WorkspaceError>(result).code, ErrorCode::StorageFailure);
    QCOMPARE(get(f.a), QByteArray("a-new")); QVERIFY(QFileInfo::exists(f.journal));
}
void RecoveryTests::committedJournalPreservesNewSet() {
    QTemporaryDir dir; auto f = interrupted(dir, true);
    auto result = storage::recover(f.a, {{dir.path()}});
    QVERIFY2(std::holds_alternative<std::monostate>(result), qPrintable(storageError(result)));
    QCOMPARE(get(f.a), QByteArray("a-new")); QCOMPARE(get(f.b), QByteArray("b-new")); QVERIFY(!QFileInfo::exists(f.journal));
}
QTEST_GUILESS_MAIN(RecoveryTests)
#include "tst_recovery.moc"
