#include "Storage.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>
#include <limits>
#include <memory>
#include <vector>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace rose::storage {
namespace {
WorkspaceError failure(ErrorCode code, const QString &message, const QString &path) {
    return {code, message, path};
}
bool within(const QString &path, const QString &root) {
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return path.compare(root, sensitivity) == 0 || path.startsWith(root.endsWith('/') ? root : root + '/', sensitivity);
}
bool approved(const QString &path, const AccessPolicy &policy) {
    for (const auto &root : policy.allowedDirectories) if (within(path, root)) return true;
    return false;
}

// All filesystem operations use a pinned, no-follow directory chain. A path
// validated earlier is not used as an unchecked authority at the write seam.
class Directory {
public:
    QString path;
#ifdef Q_OS_WIN
    std::vector<HANDLE> chain;
    ~Directory() { for (auto h : chain) CloseHandle(h); }
#else
    int fd = -1;
    ~Directory() { if (fd >= 0) ::close(fd); }
#endif
    static Outcome<std::shared_ptr<Directory>> open(const QString &target, const AccessPolicy &policy) {
        auto canonical = canonicalPath(target, policy, true);
        if (auto e = std::get_if<WorkspaceError>(&canonical)) return *e;
        const auto parent = QFileInfo(std::get<QString>(canonical)).absolutePath();
        auto result = std::make_shared<Directory>(); result->path = parent;
        if (!approved(std::get<QString>(canonical), policy)) return failure(ErrorCode::AccessDenied, "Destination is outside approved roots", target);
#ifdef Q_OS_WIN
        QStringList ancestors;
        QString current = parent;
        for (;;) {
            ancestors.prepend(current);
            const auto next = QFileInfo(current).dir().absolutePath();
            if (next == current) break;
            current = next;
        }
        for (const auto &part : ancestors) {
            const auto native = QDir::toNativeSeparators(part);
            HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (h == INVALID_HANDLE_VALUE) return failure(ErrorCode::AccessDenied, "Cannot pin approved directory", part);
            result->chain.push_back(h);
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(h, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                return failure(ErrorCode::AccessDenied, "Reparse-point directory changed during access", part);
        }
#else
        int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) return failure(ErrorCode::StorageFailure, "Cannot open filesystem root", target);
        result->fd = fd;
        for (const auto &part : parent.split('/', Qt::SkipEmptyParts)) {
            const auto encoded = QFile::encodeName(part);
            const int next = ::openat(result->fd, encoded.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (next < 0) return failure(ErrorCode::AccessDenied, "Directory changed or contains an unapproved symlink", parent);
            ::close(result->fd); result->fd = next;
        }
#endif
        return result;
    }
    Outcome<Snapshot> read(const QString &name, bool allowMissing) const {
        Snapshot snapshot; snapshot.path = QDir(path).filePath(name);
#ifdef Q_OS_WIN
        const auto native = QDir::toNativeSeparators(snapshot.path);
        HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (allowMissing && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) return snapshot;
            return failure(error == ERROR_FILE_NOT_FOUND ? ErrorCode::MissingUnit : ErrorCode::StorageFailure, "Cannot read destination", snapshot.path);
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(h, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
            CloseHandle(h); return failure(ErrorCode::AccessDenied, "Destination is not a regular approved file", snapshot.path);
        }
        QByteArray block(65536, Qt::Uninitialized); DWORD count = 0;
        for (;;) {
            if (!ReadFile(h, block.data(), DWORD(block.size()), &count, nullptr)) {
                CloseHandle(h); return failure(ErrorCode::StorageFailure, "Read failed", snapshot.path);
            }
            if (!count) break;
            snapshot.bytes.append(block.constData(), qsizetype(count));
        }
        CloseHandle(h);
#else
        const auto encoded = QFile::encodeName(name);
        const int handle = ::openat(fd, encoded.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (handle < 0) {
            if (allowMissing && errno == ENOENT) return snapshot;
            return failure(errno == ENOENT ? ErrorCode::MissingUnit : errno == ELOOP ? ErrorCode::AccessDenied : ErrorCode::StorageFailure,
                "Cannot read approved file", snapshot.path);
        }
        struct stat info{};
        if (::fstat(handle, &info) != 0 || !S_ISREG(info.st_mode)) {
            ::close(handle); return failure(ErrorCode::StorageFailure, "Destination is not a regular file", snapshot.path);
        }
        QFile file;
        if (!file.open(handle, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
            ::close(handle); return failure(ErrorCode::StorageFailure, "Cannot read file handle", snapshot.path);
        }
        snapshot.bytes = file.readAll();
        if (file.error() != QFileDevice::NoError) return failure(ErrorCode::StorageFailure, "Read failed", snapshot.path);
#endif
        snapshot.exists = true; snapshot.checksum = checksum(snapshot.bytes); return snapshot;
    }
    Status writeExclusive(const QString &name, ByteView bytes, const QString &permissionsFrom = {}) const {
        const auto destination = QDir(path).filePath(name);
#ifdef Q_OS_WIN
        const auto native = QDir::toNativeSeparators(destination);
        HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_WRITE | READ_CONTROL | WRITE_DAC,
            0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) return failure(ErrorCode::StorageFailure, "Cannot create durable staging file", destination);
        bool ok = true; qsizetype offset = 0;
        while (offset < bytes.size()) {
            DWORD written = 0; const DWORD count = DWORD(qMin<qsizetype>(bytes.size() - offset, std::numeric_limits<DWORD>::max()));
            if (!WriteFile(h, bytes.data() + offset, count, &written, nullptr) || !written) { ok = false; break; }
            offset += written;
        }
        if (ok && !permissionsFrom.isEmpty()) {
            const auto original = QDir::toNativeSeparators(QDir(path).filePath(permissionsFrom));
            DWORD needed = 0;
            GetFileSecurityW(reinterpret_cast<LPCWSTR>(original.utf16()), DACL_SECURITY_INFORMATION, nullptr, 0, &needed);
            QByteArray descriptor(qsizetype(needed), Qt::Uninitialized);
            ok = needed && GetFileSecurityW(reinterpret_cast<LPCWSTR>(original.utf16()), DACL_SECURITY_INFORMATION,
                reinterpret_cast<PSECURITY_DESCRIPTOR>(descriptor.data()), needed, &needed)
                && SetKernelObjectSecurity(h, DACL_SECURITY_INFORMATION, reinterpret_cast<PSECURITY_DESCRIPTOR>(descriptor.data()));
        }
        if (ok) ok = FlushFileBuffers(h);
        CloseHandle(h);
        if (!ok) return failure(ErrorCode::StorageFailure, "Staging write, permissions or flush failed", destination);
#else
        const auto encoded = QFile::encodeName(name);
        const int handle = ::openat(fd, encoded.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (handle < 0) return failure(ErrorCode::StorageFailure, "Cannot create durable staging file", destination);
        bool ok = true;
        if (!permissionsFrom.isEmpty()) {
            struct stat original{}; const auto from = QFile::encodeName(permissionsFrom);
            ok = ::fstatat(fd, from.constData(), &original, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(original.st_mode)
                && ::fchmod(handle, original.st_mode & 0777) == 0;
        } else {
            // New private files do not become world-readable as a save side effect.
            ok = ::fchmod(handle, 0600) == 0;
        }
        qsizetype offset = 0;
        while (ok && offset < bytes.size()) {
            const auto written = ::write(handle, bytes.data() + offset, size_t(bytes.size() - offset));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { ok = false; break; }
            offset += written;
        }
        if (ok) ok = ::fsync(handle) == 0;
        if (::close(handle) != 0) ok = false;
        if (!ok) return failure(ErrorCode::StorageFailure, "Staging write, permissions or flush failed", destination);
#endif
        return std::monostate{};
    }
    Status replace(const QString &stage, const QString &target, bool existing) const {
#ifdef Q_OS_WIN
        const auto from = QDir::toNativeSeparators(QDir(path).filePath(stage));
        const auto to = QDir::toNativeSeparators(QDir(path).filePath(target));
        const DWORD flags = MOVEFILE_WRITE_THROUGH | (existing ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()), reinterpret_cast<LPCWSTR>(to.utf16()), flags))
            return failure(ErrorCode::StorageFailure, "Durable staged replacement failed", QDir(path).filePath(target));
#else
        const auto from = QFile::encodeName(stage), to = QFile::encodeName(target);
        if (existing) {
            if (::renameat(fd, from.constData(), fd, to.constData()) != 0)
                return failure(ErrorCode::StorageFailure, "Staged replacement failed", QDir(path).filePath(target));
        } else {
            // linkat is an atomic no-clobber installation on the same filesystem.
            if (::linkat(fd, from.constData(), fd, to.constData(), 0) != 0)
                return failure(errno == EEXIST ? ErrorCode::DiskConflict : ErrorCode::StorageFailure, "New destination cannot be installed", QDir(path).filePath(target));
            if (::unlinkat(fd, from.constData(), 0) != 0)
                return failure(ErrorCode::StorageFailure, "Cannot remove installed stage", QDir(path).filePath(stage));
        }
#endif
        return sync();
    }
    Status remove(const QString &name) const {
#ifdef Q_OS_WIN
        const auto native = QDir::toNativeSeparators(QDir(path).filePath(name));
        if (!DeleteFileW(reinterpret_cast<LPCWSTR>(native.utf16())) && GetLastError() != ERROR_FILE_NOT_FOUND)
            return failure(ErrorCode::StorageFailure, "Cannot remove transaction file", QDir(path).filePath(name));
#else
        const auto encoded = QFile::encodeName(name);
        if (::unlinkat(fd, encoded.constData(), 0) != 0 && errno != ENOENT)
            return failure(ErrorCode::StorageFailure, "Cannot remove transaction file", QDir(path).filePath(name));
#endif
        return sync();
    }
    Status sync() const {
#ifndef Q_OS_WIN
        if (::fsync(fd) != 0) return failure(ErrorCode::StorageFailure, "Directory durability flush failed", path);
#endif
        // Windows replacements use MOVEFILE_WRITE_THROUGH; file data was
        // flushed before replacement. Directory fsync has no Win32 equivalent.
        return std::monostate{};
    }
};

struct Entry {
    QString target, stage, backup;
    QByteArray oldHash, newHash;
    bool existed = false;
    QString state = "pending";
};
struct Journal {
    QString model, transaction;
    bool committed = false;
    QVector<Entry> entries;
};
QByteArray journalBytes(const Journal &journal) {
    QJsonArray entries;
    for (const auto &e : journal.entries) entries.append(QJsonObject{
        {"target", e.target}, {"stage", e.stage}, {"backup", e.backup}, {"existed", e.existed},
        {"oldChecksum", QString::fromLatin1(e.oldHash.toHex())}, {"newChecksum", QString::fromLatin1(e.newHash.toHex())}, {"state", e.state}});
    const QJsonObject payload{{"schema", "rose-storage-journal/1"}, {"transaction", journal.transaction},
        {"modelPath", journal.model}, {"committed", journal.committed}, {"entries", entries}};
    const auto bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    return QJsonDocument(QJsonObject{{"payload", payload}, {"checksum", QString::fromLatin1(checksum(bytes).toHex())}}).toJson(QJsonDocument::Compact);
}
Outcome<Journal> parseJournal(const Snapshot &source, const QString &model, const AccessPolicy &policy) {
    QJsonParseError parseError; const auto document = QJsonDocument::fromJson(source.bytes, &parseError);
    const auto invalid = failure(ErrorCode::StorageFailure, "Invalid or unsupported recovery journal; preserved", source.path);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return invalid;
    const auto envelope = document.object(); const auto payload = envelope.value("payload").toObject();
    if (envelope.size() != 2 || !envelope.value("payload").isObject()
        || envelope.value("checksum").toString().toLatin1() != checksum(QJsonDocument(payload).toJson(QJsonDocument::Compact)).toHex()
        || payload.size() != 5 || payload.value("schema") != "rose-storage-journal/1"
        || payload.value("modelPath").toString() != model || !payload.value("committed").isBool()
        || !payload.value("entries").isArray()) return invalid;
    Journal result{model, payload.value("transaction").toString(), payload.value("committed").toBool(), {}};
    static const QRegularExpression uuid("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
    static const QRegularExpression digest("^[0-9a-f]{64}$");
    if (!uuid.match(result.transaction).hasMatch()) return invalid;
    QSet<QString> targets;
    const auto array = payload.value("entries").toArray(); if (array.isEmpty()) return invalid;
    for (qsizetype i = 0; i < array.size(); ++i) {
        if (!array[i].isObject()) return invalid;
        const auto item = array[i].toObject(); Entry e;
        e.target = item.value("target").toString(); e.stage = item.value("stage").toString(); e.backup = item.value("backup").toString();
        e.existed = item.value("existed").toBool(); e.state = item.value("state").toString();
        const auto oldHash = item.value("oldChecksum").toString(), newHash = item.value("newChecksum").toString();
        if (item.size() != 7 || !item.value("existed").isBool() || (e.state != "pending" && e.state != "replaced")
            || !digest.match(newHash).hasMatch() || (e.existed ? !digest.match(oldHash).hasMatch() : !oldHash.isEmpty())) return invalid;
        e.oldHash = QByteArray::fromHex(oldHash.toLatin1()); e.newHash = QByteArray::fromHex(newHash.toLatin1());
        auto target = canonicalPath(e.target, policy, true); if (auto error = std::get_if<WorkspaceError>(&target)) return *error;
        if (std::get<QString>(target) != e.target || targets.contains(e.target)) return invalid;
        targets.insert(e.target);
        const auto prefix = QDir(QFileInfo(e.target).absolutePath()).filePath(".rose-txn-" + result.transaction + '-' + QString::number(i));
        if (e.stage != prefix + ".stage" || (e.existed ? e.backup != prefix + ".backup" : !e.backup.isEmpty())) return invalid;
        result.entries.append(std::move(e));
    }
    return result;
}
Status checkExpected(const Snapshot &snapshot, const std::optional<QByteArray> &expected) {
    if (expected ? (!snapshot.exists || snapshot.checksum != *expected) : snapshot.exists)
        return failure(ErrorCode::DiskConflict, "Destination changed since it was loaded; no overwrite performed", snapshot.path);
    return std::monostate{};
}
Status writeJournal(const Journal &journal, const AccessPolicy &policy, std::optional<QByteArray> expected) {
    const auto path = journalPath(journal.model);
    auto opened = Directory::open(path, policy); if (auto e = std::get_if<WorkspaceError>(&opened)) return *e;
    const auto dir = std::get<std::shared_ptr<Directory>>(opened); const auto name = QFileInfo(path).fileName();
    const auto stage = name + '.' + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".stage";
    auto written = dir->writeExclusive(stage, journalBytes(journal)); if (auto e = std::get_if<WorkspaceError>(&written)) return *e;
    auto current = dir->read(name, true); if (auto e = std::get_if<WorkspaceError>(&current)) { dir->remove(stage); return *e; }
    auto checked = checkExpected(std::get<Snapshot>(current), expected);
    if (auto e = std::get_if<WorkspaceError>(&checked)) { dir->remove(stage); return *e; }
    auto replaced = dir->replace(stage, name, expected.has_value());
    if (std::holds_alternative<WorkspaceError>(replaced)) dir->remove(stage);
    return replaced;
}
Status removeArtifact(const QString &path, const QByteArray &expected, const AccessPolicy &policy) {
    auto opened = Directory::open(path, policy); if (auto e = std::get_if<WorkspaceError>(&opened)) return *e;
    auto dir = std::get<std::shared_ptr<Directory>>(opened); const auto name = QFileInfo(path).fileName();
    auto read = dir->read(name, true); if (auto e = std::get_if<WorkspaceError>(&read)) return *e;
    const auto &snapshot = std::get<Snapshot>(read); if (!snapshot.exists) return std::monostate{};
    if (snapshot.checksum != expected) return failure(ErrorCode::DiskConflict, "Transaction artifact changed; preserved", path);
    return dir->remove(name);
}
} // namespace

QByteArray checksum(ByteView bytes) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    // Qt 5's incremental API takes int lengths; chunk without copying on Qt 6 too.
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        const auto count = int(qMin<qsizetype>(bytes.size() - offset, std::numeric_limits<int>::max()));
        hash.addData(bytes.data() + offset, count);
        offset += count;
    }
    return hash.result();
}
Outcome<AccessPolicy> canonicalPolicy(const AccessPolicy &input) {
    AccessPolicy result = input; result.allowedDirectories.clear();
    for (const auto &path : input.allowedDirectories) {
        const QFileInfo info(path); const auto canonical = info.canonicalFilePath();
        if (canonical.isEmpty() || !info.isDir()) return failure(ErrorCode::AccessDenied, "Approved root must be an existing directory", path);
        if (!result.allowedDirectories.contains(canonical)) result.allowedDirectories.append(canonical);
    }
    if (result.allowedDirectories.isEmpty()) return failure(ErrorCode::AccessDenied, "No approved directories", {});
    return result;
}
Outcome<QString> canonicalPath(const QString &path, const AccessPolicy &policy, bool allowMissing) {
    if (path.isEmpty() || path.contains(QChar(0))) return failure(ErrorCode::AccessDenied, "Empty or invalid path", path);
    QFileInfo info(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    auto canonical = info.canonicalFilePath();
    if (canonical.isEmpty()) {
        if (info.isSymLink()) return failure(ErrorCode::AccessDenied, "Broken symlink is not an approved destination", path);
        const auto parent = QFileInfo(info.absolutePath()).canonicalFilePath();
        if (parent.isEmpty()) return failure(ErrorCode::MissingUnit, "Parent directory does not exist", path);
        canonical = QDir(parent).filePath(info.fileName());
        if (!approved(canonical, policy)) return failure(ErrorCode::AccessDenied, "Resolved path is outside approved directories", path);
        if (!allowMissing) return failure(ErrorCode::MissingUnit, "Referenced file does not exist", canonical);
    }
    if (!approved(canonical, policy)) return failure(ErrorCode::AccessDenied, "Resolved path is outside approved directories", path);
    return canonical;
}
Outcome<Snapshot> read(const QString &path, const AccessPolicy &input, bool allowMissing) {
    auto normalized = canonicalPolicy(input); if (auto e = std::get_if<WorkspaceError>(&normalized)) return *e;
    const auto &policy = std::get<AccessPolicy>(normalized);
    auto canonical = canonicalPath(path, policy, allowMissing); if (auto e = std::get_if<WorkspaceError>(&canonical)) return *e;
    auto opened = Directory::open(std::get<QString>(canonical), policy); if (auto e = std::get_if<WorkspaceError>(&opened)) return *e;
    return std::get<std::shared_ptr<Directory>>(opened)->read(QFileInfo(std::get<QString>(canonical)).fileName(), allowMissing);
}
QString journalPath(const QString &modelPath) {
    const QFileInfo info(modelPath); return QDir(info.absolutePath()).filePath('.' + info.fileName() + ".rose-journal.json");
}
Status recover(const QString &modelPath, const AccessPolicy &input) {
    auto normalized = canonicalPolicy(input); if (auto e = std::get_if<WorkspaceError>(&normalized)) return *e;
    const auto &policy = std::get<AccessPolicy>(normalized);
    auto model = canonicalPath(modelPath, policy, true); if (auto e = std::get_if<WorkspaceError>(&model)) return *e;
    auto source = read(journalPath(std::get<QString>(model)), policy, true); if (auto e = std::get_if<WorkspaceError>(&source)) return *e;
    if (!std::get<Snapshot>(source).exists) return std::monostate{};
    if (!policy.writable) return failure(ErrorCode::AccessDenied, "Recovery requires explicit write access", journalPath(modelPath));
    auto parsed = parseJournal(std::get<Snapshot>(source), std::get<QString>(model), policy); if (auto e = std::get_if<WorkspaceError>(&parsed)) return *e;
    const auto &journal = std::get<Journal>(parsed);
    // Preflight the entire set before performing any rollback. A single external
    // change preserves every recoverable artifact and prevents partial recovery.
    for (const auto &e : journal.entries) {
        auto current = read(e.target, policy, true); if (auto error = std::get_if<WorkspaceError>(&current)) return *error;
        const auto &snapshot = std::get<Snapshot>(current);
        const bool old = e.existed ? snapshot.exists && snapshot.checksum == e.oldHash : !snapshot.exists;
        const bool next = snapshot.exists && snapshot.checksum == e.newHash;
        if (journal.committed ? !next : (!old && !next)) return failure(ErrorCode::DiskConflict, "Recovery target was externally changed; journal preserved", e.target);
        if (e.existed) {
            auto backup = read(e.backup, policy, true); if (auto error = std::get_if<WorkspaceError>(&backup)) return *error;
            const auto &b = std::get<Snapshot>(backup);
            if (!journal.committed && (!b.exists || b.checksum != e.oldHash)) return failure(ErrorCode::DiskConflict, "Recovery backup is missing or changed", e.backup);
            if (b.exists && b.checksum != e.oldHash) return failure(ErrorCode::DiskConflict, "Recovery backup was externally changed", e.backup);
        }
        auto stage = read(e.stage, policy, true); if (auto error = std::get_if<WorkspaceError>(&stage)) return *error;
        const auto &s = std::get<Snapshot>(stage);
        if (s.exists && s.checksum != e.newHash) return failure(ErrorCode::DiskConflict, "Recovery stage was externally changed", e.stage);
    }
    if (!journal.committed) for (qsizetype i = 0; i < journal.entries.size(); ++i) {
        const auto &e = journal.entries[i];
        auto opened = Directory::open(e.target, policy); if (auto error = std::get_if<WorkspaceError>(&opened)) return *error;
        const auto dir = std::get<std::shared_ptr<Directory>>(opened); const auto name = QFileInfo(e.target).fileName();
        auto current = dir->read(name, true); if (auto error = std::get_if<WorkspaceError>(&current)) return *error;
        const auto &snapshot = std::get<Snapshot>(current);
        const bool old = e.existed ? snapshot.exists && snapshot.checksum == e.oldHash : !snapshot.exists;
        if (old) continue;
        if (!snapshot.exists || snapshot.checksum != e.newHash) return failure(ErrorCode::DiskConflict, "Recovery target changed during rollback", e.target);
        if (e.existed) {
            auto backup = dir->read(QFileInfo(e.backup).fileName(), false); if (auto error = std::get_if<WorkspaceError>(&backup)) return *error;
            if (std::get<Snapshot>(backup).checksum != e.oldHash) return failure(ErrorCode::DiskConflict, "Backup changed during rollback", e.backup);
            // Keep the durable backup until the complete rollback finishes.
            const auto rollback = QFileInfo(e.backup).fileName() + ".rollback-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
            auto staged = dir->writeExclusive(rollback, std::get<Snapshot>(backup).bytes, name); if (auto error = std::get_if<WorkspaceError>(&staged)) return *error;
            auto replaced = dir->replace(rollback, name, true); if (auto error = std::get_if<WorkspaceError>(&replaced)) return *error;
        } else {
            auto removed = dir->remove(name); if (auto error = std::get_if<WorkspaceError>(&removed)) return *error;
        }
    }
    // Removing the journal first makes interrupted cleanup harmless. The set is
    // already restored/committed and all remaining files are private artifacts.
    auto removed = removeArtifact(std::get<Snapshot>(source).path, std::get<Snapshot>(source).checksum, policy);
    if (auto e = std::get_if<WorkspaceError>(&removed)) return *e;
    for (const auto &e : journal.entries) {
        if (e.existed) { auto status = removeArtifact(e.backup, e.oldHash, policy); if (auto error = std::get_if<WorkspaceError>(&status)) return *error; }
        auto status = removeArtifact(e.stage, e.newHash, policy); if (auto error = std::get_if<WorkspaceError>(&status)) return *error;
    }
    return std::monostate{};
}
Outcome<QMap<QString, QByteArray>> save(const QVector<Write> &writes, const QString &modelPath, const AccessPolicy &input) {
    auto normalized = canonicalPolicy(input); if (auto e = std::get_if<WorkspaceError>(&normalized)) return *e;
    const auto &policy = std::get<AccessPolicy>(normalized);
    if (!policy.writable) return failure(ErrorCode::AccessDenied, "Write access was not approved", modelPath);
    auto recovered = recover(modelPath, policy); if (auto e = std::get_if<WorkspaceError>(&recovered)) return *e;
    auto canonical = canonicalPath(modelPath, policy, true); if (auto e = std::get_if<WorkspaceError>(&canonical)) return *e;
    Journal journal{std::get<QString>(canonical), QUuid::createUuid().toString(QUuid::WithoutBraces), false, {}};
    QMap<QString, QByteArray> result; QVector<Snapshot> originals;
    QSet<QString> targets;
    for (const auto &write : writes) {
        auto current = read(write.path, policy, true); if (auto e = std::get_if<WorkspaceError>(&current)) return *e;
        auto snapshot = std::move(std::get<Snapshot>(current));
        if (targets.contains(snapshot.path)) return failure(ErrorCode::StorageFailure, "Duplicate destination in save set", snapshot.path);
        targets.insert(snapshot.path);
        auto checked = checkExpected(snapshot, write.expectedChecksum); if (auto e = std::get_if<WorkspaceError>(&checked)) return *e;
        const auto nextHash = checksum(write.bytes); result.insert(snapshot.path, nextHash);
        if (snapshot.exists && nextHash == snapshot.checksum) continue;
        const auto index = journal.entries.size();
        const auto prefix = QDir(QFileInfo(snapshot.path).absolutePath()).filePath(".rose-txn-" + journal.transaction + '-' + QString::number(index));
        journal.entries.append({snapshot.path, prefix + ".stage", snapshot.exists ? prefix + ".backup" : QString{}, snapshot.checksum, nextHash, snapshot.exists, "pending"});
        originals.append(std::move(snapshot));
    }
    if (journal.entries.isEmpty()) return result;
    auto cleanPreJournal = [&]() {
        for (const auto &e : journal.entries) {
            removeArtifact(e.stage, e.newHash, policy);
            if (e.existed) removeArtifact(e.backup, e.oldHash, policy);
        }
    };
    for (qsizetype i = 0; i < journal.entries.size(); ++i) {
        const auto &e = journal.entries[i]; auto opened = Directory::open(e.target, policy);
        if (auto error = std::get_if<WorkspaceError>(&opened)) { cleanPreJournal(); return *error; }
        const auto dir = std::get<std::shared_ptr<Directory>>(opened); const auto name = QFileInfo(e.target).fileName();
        if (e.existed && journal.entries.size() > 1) {
            auto backed = dir->writeExclusive(QFileInfo(e.backup).fileName(), originals[i].bytes, name);
            if (auto error = std::get_if<WorkspaceError>(&backed)) { cleanPreJournal(); return *error; }
        }
        const Write *write = nullptr;
        for (const auto &candidate : writes) {
            auto path = canonicalPath(candidate.path, policy, true);
            if (std::holds_alternative<QString>(path) && std::get<QString>(path) == e.target) { write = &candidate; break; }
        }
        if (!write) { cleanPreJournal(); return failure(ErrorCode::AccessDenied, "Destination path changed during staging", e.target); }
        auto staged = dir->writeExclusive(QFileInfo(e.stage).fileName(), write->bytes, e.existed ? name : QString{});
        if (auto error = std::get_if<WorkspaceError>(&staged)) { cleanPreJournal(); return *error; }
        auto synced = dir->sync(); if (auto error = std::get_if<WorkspaceError>(&synced)) { cleanPreJournal(); return *error; }
    }
    QByteArray journalHash;
    if (journal.entries.size() > 1) {
        auto recorded = writeJournal(journal, policy, std::nullopt);
        if (auto e = std::get_if<WorkspaceError>(&recorded)) {
            // A flush failure may have installed a journal: do not discard its backups.
            auto current = read(journalPath(journal.model), policy, true);
            if (std::holds_alternative<Snapshot>(current) && !std::get<Snapshot>(current).exists) cleanPreJournal();
            return *e;
        }
        journalHash = checksum(journalBytes(journal));
    }
    for (auto &e : journal.entries) {
        auto opened = Directory::open(e.target, policy); if (auto error = std::get_if<WorkspaceError>(&opened)) { if (journalHash.isEmpty()) cleanPreJournal(); return *error; }
        const auto dir = std::get<std::shared_ptr<Directory>>(opened); const auto name = QFileInfo(e.target).fileName();
        auto current = dir->read(name, true); if (auto error = std::get_if<WorkspaceError>(&current)) { if (journalHash.isEmpty()) cleanPreJournal(); return *error; }
        auto checked = checkExpected(std::get<Snapshot>(current), e.existed ? std::optional<QByteArray>(e.oldHash) : std::nullopt);
        if (auto error = std::get_if<WorkspaceError>(&checked)) { if (journalHash.isEmpty()) cleanPreJournal(); return *error; }
        auto staged = dir->read(QFileInfo(e.stage).fileName(), false); if (auto error = std::get_if<WorkspaceError>(&staged)) return *error;
        if (std::get<Snapshot>(staged).checksum != e.newHash) return failure(ErrorCode::DiskConflict, "Staged content was changed", e.stage);
        auto replaced = dir->replace(QFileInfo(e.stage).fileName(), name, e.existed); if (auto error = std::get_if<WorkspaceError>(&replaced)) return *error;
        e.state = "replaced";
        if (!journalHash.isEmpty()) {
            auto recorded = writeJournal(journal, policy, journalHash); if (auto error = std::get_if<WorkspaceError>(&recorded)) return *error;
            journalHash = checksum(journalBytes(journal));
        }
    }
    if (!journalHash.isEmpty()) {
        journal.committed = true;
        auto recorded = writeJournal(journal, policy, journalHash); if (auto error = std::get_if<WorkspaceError>(&recorded)) return *error;
        auto cleaned = recover(journal.model, policy); if (auto error = std::get_if<WorkspaceError>(&cleaned)) return *error;
    }
    return result;
}
} // namespace rose::storage
