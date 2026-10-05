#include "AgentHistory.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <array>
#endif
namespace rose::agent {
namespace {
#ifdef Q_OS_WIN
// Qt's Windows permission bits only toggle the read-only attribute. The
// inheritable DACL makes even QSaveFile's exclusive temporary file private.
class WindowsHistorySecurity final {
public:
    WindowsHistorySecurity() = default;
    ~WindowsHistorySecurity() { if (directory_ != INVALID_HANDLE_VALUE) CloseHandle(directory_); }
    WindowsHistorySecurity(const WindowsHistorySecurity &) = delete;
    WindowsHistorySecurity &operator=(const WindowsHistorySecurity &) = delete;

    DWORD prepare(const QString &path) {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return GetLastError();
        DWORD needed = 0;
        const bool loaded = GetTokenInformation(token, TokenUser, user_.data(), DWORD(user_.size()), &needed);
        const auto tokenError = loaded ? ERROR_SUCCESS : GetLastError();
        CloseHandle(token);
        if (!loaded) return tokenError;
        if (auto error = makeAcl(true); error != ERROR_SUCCESS) return error;
        SECURITY_DESCRIPTOR descriptor{};
        if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION)
            || !SetSecurityDescriptorDacl(&descriptor, TRUE, acl(), FALSE)
            || !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) return GetLastError();
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
        const auto native = QDir::toNativeSeparators(path);
        const auto name = reinterpret_cast<LPCWSTR>(native.utf16());
        if (!CreateDirectoryW(name, &attributes)) {
            const auto error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS) return error;
        }
        // Pin the directory through the atomic write; reject links/reparse points.
        directory_ = CreateFileW(name, FILE_READ_ATTRIBUTES | READ_CONTROL | WRITE_DAC, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (directory_ == INVALID_HANDLE_VALUE) return GetLastError();
        if (auto error = checkType(directory_, true); error != ERROR_SUCCESS) return error;
        return protect(directory_);
    }

    DWORD protectFile(const QString &path, bool mayBeMissing = false) {
        const auto native = QDir::toNativeSeparators(path);
        const auto file = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()),
            FILE_READ_ATTRIBUTES | READ_CONTROL | WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            return mayBeMissing && error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error;
        }
        auto error = checkType(file, false);
        if (error == ERROR_SUCCESS) error = makeAcl(false);
        if (error == ERROR_SUCCESS) error = protect(file);
        CloseHandle(file);
        return error;
    }
private:
    DWORD makeAcl(bool directory) {
        if (!InitializeAcl(acl(), DWORD(acl_.size()), ACL_REVISION)
            || !AddAccessAllowedAceEx(acl(), ACL_REVISION,
                directory ? OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE : 0, FILE_ALL_ACCESS,
                reinterpret_cast<TOKEN_USER *>(user_.data())->User.Sid)) return GetLastError();
        return ERROR_SUCCESS;
    }
    PACL acl() { return reinterpret_cast<PACL>(acl_.data()); }
    DWORD protect(HANDLE object) {
        return SetSecurityInfo(object, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, acl(), nullptr);
    }
    static DWORD checkType(HANDLE object, bool directory) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(object, &info)) return GetLastError();
        if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            || bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory) return ERROR_ACCESS_DENIED;
        return ERROR_SUCCESS;
    }
    alignas(TOKEN_USER) std::array<unsigned char, sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE> user_{};
    alignas(ACL) std::array<unsigned char, sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + SECURITY_MAX_SID_SIZE> acl_{};
    HANDLE directory_ = INVALID_HANDLE_VALUE;
};
AgentError historySecurityError(DWORD code) {
    return {AgentErrorCode::Configuration,
        QString("Could not secure private local agent history (Windows error %1); model data is unaffected.").arg(code)};
}
#endif
}
std::optional<AgentError> AgentHistory::record(const ProviderConfig &config, RunId run, RunStatus status, const QString &proposal, const QString &transaction) {
    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty()) return AgentError{AgentErrorCode::Configuration, "No per-user local agent history location is available."};
    const auto directory = base + "/agent-history";
#ifdef Q_OS_WIN
    if (!QDir().mkpath(base)) return AgentError{AgentErrorCode::Configuration, "Could not create per-user local agent history storage."};
    WindowsHistorySecurity security;
    if (auto error = security.prepare(directory); error != ERROR_SUCCESS) return historySecurityError(error);
    if (auto error = security.protectFile(directory + "/history.json", true); error != ERROR_SUCCESS) return historySecurityError(error);
#else
    if (!QDir().mkpath(directory) || !QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
        return AgentError{AgentErrorCode::Configuration, "Could not secure local agent history directory."};
#endif
    QJsonArray entries;
    QFile previous(directory + "/history.json");
    if (previous.exists()) {
        if (!previous.open(QIODevice::ReadOnly) || previous.size() > 1024 * 1024)
            return AgentError{AgentErrorCode::Configuration, "Could not read bounded local agent history."};
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(previous.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isArray())
            return AgentError{AgentErrorCode::Configuration, "Local agent history is invalid; model data is unaffected."};
        entries = document.array();
        // Windows readers do not share delete access; release before replacing.
        previous.close();
    }
    while (entries.size() >= 500) entries.removeFirst();
    // No prompts, model content, keys, endpoints with credentials, or hidden reasoning.
    entries.append(QJsonObject{{"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}, {"runId", QString::number(run)},
        {"status", statusName(status)}, {"provider", config.kind == ProviderKind::Local ? "local" : "hosted"},
        {"model", config.model}, {"proposalId", proposal}, {"transactionId", transaction}});
    QSaveFile file(directory + "/history.json");
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return AgentError{AgentErrorCode::Configuration, "Could not open restrictive local agent history."};
#ifndef Q_OS_WIN
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return AgentError{AgentErrorCode::Configuration, "Could not secure restrictive local agent history."};
#endif
    const auto bytes = QJsonDocument(entries).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit())
        return AgentError{AgentErrorCode::Configuration, "Could not persist local agent history; model data is unaffected."};
#ifdef Q_OS_WIN
    if (auto error = security.protectFile(directory + "/history.json"); error != ERROR_SUCCESS) return historySecurityError(error);
#endif
    return std::nullopt;
}
} // namespace rose::agent
