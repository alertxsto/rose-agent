#include "Credentials.h"
#include <QFutureInterface>
#include <QtConcurrentRun>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <wincred.h>
#elif defined(Q_OS_LINUX)
#include <QDBusArgument>
#include <QDBusError>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusPendingCallWatcher>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#endif

namespace rose::agent {
namespace {
AgentError unavailable(const QString &detail) { return {AgentErrorCode::Credentials, detail}; }
using CredentialResult = std::shared_ptr<CredentialLookup>;
thread_local QFutureInterface<CredentialResult> *lookupFuture = nullptr;
bool lookupCancelled() { return lookupFuture && lookupFuture->isCanceled(); }
bool validReference(const QString &reference) {
    if (reference.isEmpty() || reference.size() > 200) return false;
    for (auto c : reference) if (!c.isLetterOrNumber() && c != '-' && c != '_' && c != '.') return false;
    return true;
}
#ifdef Q_OS_WIN
QString target(const QString &reference) { return "RoseAgent/provider/" + reference; }
#elif defined(Q_OS_LINUX)
constexpr auto service = "org.freedesktop.secrets";
constexpr auto root = "/org/freedesktop/secrets";
constexpr auto serviceInterface = "org.freedesktop.Secret.Service";
struct Secret { QDBusObjectPath session; QByteArray parameters, value; QString contentType; };
QDBusArgument &operator<<(QDBusArgument &argument, const Secret &secret) {
    argument.beginStructure(); argument << secret.session << secret.parameters << secret.value << secret.contentType; argument.endStructure(); return argument;
}
const QDBusArgument &operator>>(const QDBusArgument &argument, Secret &secret) {
    argument.beginStructure(); argument >> secret.session >> secret.parameters >> secret.value >> secret.contentType; argument.endStructure(); return argument;
}
class PromptReceiver final : public QObject {
    Q_OBJECT
public:
    QEventLoop loop;
    bool received = false, dismissed = true;
    QVariant result;
public slots:
    void completed(bool cancelled, const QDBusVariant &value) {
        received = true; dismissed = cancelled; result = value.variant(); loop.quit();
    }
};
QDBusMessage call(const QString &path, const QString &interfaceName, const QString &method, const QList<QVariant> &arguments = {});
void dismissPrompt(const QString &path) {
    QDBusConnection::sessionBus().asyncCall(
        QDBusMessage::createMethodCall(service, path, "org.freedesktop.Secret.Prompt", "Dismiss"), 10000);
}
void cleanupCancelledReply(const QDBusMessage &reply, const QString &path, const QString &method) {
    if (method == "Prompt") {
        // A delayed Prompt acknowledgement may arrive after the first Dismiss.
        dismissPrompt(path);
    } else if (method == "OpenSession" && reply.type() == QDBusMessage::ReplyMessage && reply.arguments().size() == 2) {
        const auto sessionPath = qvariant_cast<QDBusObjectPath>(reply.arguments()[1]).path();
        if (!sessionPath.isEmpty() && sessionPath != "/")
            QDBusConnection::sessionBus().asyncCall(QDBusMessage::createMethodCall(
                service, sessionPath, "org.freedesktop.Secret.Session", "Close"), 10000);
    }
}
void retainCancelledReply(const QDBusPendingCall &pending, const QString &path, const QString &method) {
    if (method != "OpenSession" && method != "Prompt") return;
    auto *watcher = new QDBusPendingCallWatcher(pending);
    if (watcher->isFinished()) {
        cleanupCancelledReply(watcher->reply(), path, method);
        delete watcher;
        return;
    }
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher,
        [path, method](QDBusPendingCallWatcher *finished) {
            cleanupCancelledReply(finished->reply(), path, method);
            finished->deleteLater();
        });
    // The lookup's pool thread stops processing events when cancellation returns.
    watcher->moveToThread(QCoreApplication::instance()->thread());
}
std::variant<QVariant, AgentError> prompt(const QDBusObjectPath &path) {
    if (path.path() == "/") return QVariant{};
    auto bus = QDBusConnection::sessionBus();
    PromptReceiver receiver;
    if (!bus.connect(service, path.path(), "org.freedesktop.Secret.Prompt", "Completed", &receiver, SLOT(completed(bool,QDBusVariant)))) {
        dismissPrompt(path.path());
        return unavailable("Cannot receive the Secret Service unlock prompt.");
    }
    auto reply = call(path.path(), "org.freedesktop.Secret.Prompt", "Prompt", {QString{}});
    if (reply.type() == QDBusMessage::ErrorMessage) {
        dismissPrompt(path.path());
        return unavailable("Secret Service prompt failed or was cancelled. Unlock your desktop keyring.");
    }
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &receiver.loop, &QEventLoop::quit);
    timer.start(120000);
    QTimer cancellation;
    if (lookupFuture) {
        cancellation.setInterval(25);
        QObject::connect(&cancellation, &QTimer::timeout, &receiver.loop, [&receiver] {
            if (lookupCancelled()) receiver.loop.quit();
        });
        cancellation.start();
    }
    if (!receiver.received && !lookupCancelled()) receiver.loop.exec();
    if (!receiver.received || lookupCancelled()) {
        dismissPrompt(path.path());
        return unavailable(lookupCancelled() ? "Credential lookup cancelled." : "Secret Service prompt timed out. Unlock your desktop keyring and try again.");
    }
    if (receiver.dismissed) return unavailable("Secret Service unlock was cancelled.");
    return receiver.result;
}
QDBusMessage call(const QString &path, const QString &interfaceName, const QString &method, const QList<QVariant> &arguments) {
    if (lookupFuture) {
        auto message = QDBusMessage::createMethodCall(service, path, interfaceName, method);
        message.setArguments(arguments);
        const bool cleanup = method == "Close" || method == "Dismiss";
        if (lookupCancelled()) {
            if (cleanup) QDBusConnection::sessionBus().asyncCall(message, 10000);
            return QDBusMessage::createError(QDBusError::NoReply, "Credential lookup cancelled.");
        }
        QDBusPendingCallWatcher pending(QDBusConnection::sessionBus().asyncCall(message, 10000));
        QEventLoop loop;
        QObject::connect(&pending, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
        QTimer cancellation;
        cancellation.setInterval(25);
        QObject::connect(&cancellation, &QTimer::timeout, &loop, [&loop] {
            if (lookupCancelled()) loop.quit();
        });
        cancellation.start();
        if (!pending.isFinished() && !lookupCancelled()) loop.exec();
        if (lookupCancelled()) {
            retainCancelledReply(pending, path, method);
            return QDBusMessage::createError(QDBusError::NoReply, "Credential lookup cancelled.");
        }
        return pending.reply();
    }
    QDBusInterface interface(service, path, interfaceName, QDBusConnection::sessionBus());
    interface.setTimeout(10000);
    return interface.callWithArgumentList(QDBus::Block, method, arguments);
}
bool validReply(const QDBusMessage &reply, int count) { return reply.type() == QDBusMessage::ReplyMessage && reply.arguments().size() == count; }
std::variant<QDBusObjectPath, AgentError> session() {
    const auto reply = call(root, serviceInterface, "OpenSession", {QString("plain"), QVariant::fromValue(QDBusVariant(QString{}))});
    if (!validReply(reply, 2)) return unavailable("Linux Secret Service is unavailable. Start/unlock a compatible desktop keyring; no plaintext fallback is used.");
    return qvariant_cast<QDBusObjectPath>(reply.arguments()[1]);
}
std::variant<QList<QDBusObjectPath>, AgentError> search(const QString &reference) {
    static const auto attributesType = qDBusRegisterMetaType<QMap<QString, QString>>();
    Q_UNUSED(attributesType);
    const QMap<QString, QString> attributes{{"application", "rose-agent"}, {"reference", reference}};
    const auto reply = call(root, serviceInterface, "SearchItems", {QVariant::fromValue(attributes)});
    if (!validReply(reply, 2)) return unavailable("Secret Service credential lookup failed.");
    auto unlocked = qdbus_cast<QList<QDBusObjectPath>>(reply.arguments()[0]);
    const auto locked = qdbus_cast<QList<QDBusObjectPath>>(reply.arguments()[1]);
    unlocked.append(locked);
    return unlocked;
}
std::optional<AgentError> unlock(const QList<QDBusObjectPath> &paths) {
    const auto reply = call(root, serviceInterface, "Unlock", {QVariant::fromValue(paths)});
    if (!validReply(reply, 2)) return unavailable("Secret Service unlock failed.");
    const auto result = prompt(qvariant_cast<QDBusObjectPath>(reply.arguments()[1]));
    if (auto error = std::get_if<AgentError>(&result)) return *error;
    return std::nullopt;
}
#endif
}
} // namespace rose::agent

#ifdef Q_OS_LINUX
Q_DECLARE_METATYPE(rose::agent::Secret)
#endif

namespace rose::agent {
std::variant<QByteArray, AgentError> Credentials::read(const QString &reference) {
    if (!validReference(reference)) return unavailable("Invalid credential reference.");
#ifdef Q_OS_WIN
    const auto name = target(reference).toStdWString();
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(name.c_str(), CRED_TYPE_GENERIC, 0, &credential))
        return unavailable("Windows Credential Manager lookup failed. Store a provider key in settings.");
    QByteArray key(reinterpret_cast<const char *>(credential->CredentialBlob), credential->CredentialBlobSize);
    if (credential->CredentialBlobSize)
        SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
    CredFree(credential);
    return key;
#elif defined(Q_OS_LINUX)
    qDBusRegisterMetaType<Secret>();
    const auto opened = session();
    if (auto error = std::get_if<AgentError>(&opened)) return *error;
    const auto sessionPath = std::get<QDBusObjectPath>(opened);
    const auto items = search(reference);
    if (auto error = std::get_if<AgentError>(&items)) { call(sessionPath.path(), "org.freedesktop.Secret.Session", "Close"); return *error; }
    const auto paths = std::get<QList<QDBusObjectPath>>(items);
    if (paths.size() != 1) { call(sessionPath.path(), "org.freedesktop.Secret.Session", "Close"); return unavailable("Provider credential is missing or ambiguous in Secret Service. Store it again."); }
    if (auto error = unlock(paths)) { call(sessionPath.path(), "org.freedesktop.Secret.Session", "Close"); return *error; }
    const auto reply = call(paths.first().path(), "org.freedesktop.Secret.Item", "GetSecret", {QVariant::fromValue(sessionPath)});
    call(sessionPath.path(), "org.freedesktop.Secret.Session", "Close");
    if (!validReply(reply, 1)) return unavailable("Secret Service could not read the provider credential.");
    return qdbus_cast<Secret>(reply.arguments()[0]).value;
#else
    return unavailable("This platform has no supported OS credential adapter.");
#endif
}
QFuture<std::shared_ptr<CredentialLookup>> Credentials::readAsync(const QString &reference) {
    // Basic QtConcurrent::run futures cannot be cancelled in Qt 5. Own the
    // public promise interface explicitly so both Qt majors signal cancellation
    // to the OS lookup and finish only after its cleanup has completed.
    QFutureInterface<CredentialResult> promise;
    promise.reportStarted();
    auto future = promise.future();
    (void)QtConcurrent::run([reference, promise]() mutable {
        struct Binding {
            explicit Binding(QFutureInterface<CredentialResult> &value) { lookupFuture = &value; }
            ~Binding() { lookupFuture = nullptr; }
        } binding(promise);
        if (!promise.isCanceled()) {
            auto result = std::make_shared<CredentialLookup>(read(reference));
            if (!promise.isCanceled()) promise.reportResult(result);
            if (promise.isCanceled()) result->wipe();
        }
        promise.reportFinished();
    });
    return future;
}
std::optional<AgentError> Credentials::store(const QString &reference, const QByteArray &key) {
    if (!validReference(reference) || key.isEmpty() || key.size() > 4096 || key.contains('\r') || key.contains('\n'))
        return unavailable("Credential reference/key is empty, oversized, or invalid.");
#ifdef Q_OS_WIN
    const auto name = target(reference).toStdWString();
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t *>(name.c_str());
    credential.CredentialBlobSize = DWORD(key.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(key.constData()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (!CredWriteW(&credential, 0)) return unavailable("Windows Credential Manager could not store the provider key.");
    return std::nullopt;
#elif defined(Q_OS_LINUX)
    qDBusRegisterMetaType<Secret>();
    static const auto attributesType = qDBusRegisterMetaType<QMap<QString, QString>>();
    Q_UNUSED(attributesType);
    const auto opened = session();
    if (auto error = std::get_if<AgentError>(&opened)) return *error;
    const auto sessionPath = std::get<QDBusObjectPath>(opened);
    auto collectionReply = call(root, serviceInterface, "ReadAlias", {QString("default")});
    if (!validReply(collectionReply, 1) || qvariant_cast<QDBusObjectPath>(collectionReply.arguments()[0]).path() == "/") {
        call(sessionPath.path(), "org.freedesktop.Secret.Session", "Close");
        return unavailable("Secret Service has no default collection. Create a desktop keyring first.");
    }
    const auto collection = qvariant_cast<QDBusObjectPath>(collectionReply.arguments()[0]);
    if (auto error = unlock({collection})) { call(sessionPath.path(), "org.freedesktop.Secret.Session", "Close"); return *error; }
    const QMap<QString, QString> attributes{{"application", "rose-agent"}, {"reference", reference}};
    const QVariantMap properties{{"org.freedesktop.Secret.Item.Label", "Rose Agent provider credential"},
        {"org.freedesktop.Secret.Item.Attributes", QVariant::fromValue(attributes)}};
    const Secret secret{sessionPath, {}, key, "text/plain; charset=utf-8"};
    auto reply = call(collection.path(), "org.freedesktop.Secret.Collection", "CreateItem",
        {properties, QVariant::fromValue(secret), true});
    if (!validReply(reply, 2)) { call(sessionPath.path(), "org.freedesktop.Secret.Session", "Close"); return unavailable("Secret Service could not store the provider credential."); }
    auto result = prompt(qvariant_cast<QDBusObjectPath>(reply.arguments()[1]));
    call(sessionPath.path(), "org.freedesktop.Secret.Session", "Close");
    if (auto error = std::get_if<AgentError>(&result)) return *error;
    return std::nullopt;
#else
    return unavailable("This platform has no supported OS credential adapter.");
#endif
}
std::optional<AgentError> Credentials::remove(const QString &reference) {
    if (!validReference(reference)) return unavailable("Invalid credential reference.");
#ifdef Q_OS_WIN
    const auto name = target(reference).toStdWString();
    if (!CredDeleteW(name.c_str(), CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND)
        return unavailable("Windows Credential Manager could not remove the credential.");
    return std::nullopt;
#elif defined(Q_OS_LINUX)
    const auto items = search(reference);
    if (auto error = std::get_if<AgentError>(&items)) return *error;
    for (const auto &path : std::get<QList<QDBusObjectPath>>(items)) {
        const auto reply = call(path.path(), "org.freedesktop.Secret.Item", "Delete");
        if (!validReply(reply, 1)) return unavailable("Secret Service could not remove the credential.");
        const auto result = prompt(qvariant_cast<QDBusObjectPath>(reply.arguments()[0]));
        if (auto error = std::get_if<AgentError>(&result)) return *error;
    }
    return std::nullopt;
#else
    return unavailable("This platform has no supported OS credential adapter.");
#endif
}
} // namespace rose::agent
#ifdef Q_OS_LINUX
#include "Credentials.moc"
#endif
