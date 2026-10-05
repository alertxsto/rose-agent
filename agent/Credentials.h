#pragma once
#include "AgentTypes.h"
#include <QFuture>
#include <memory>

namespace rose::agent {
// No settings/file fallback. References, never keys, may be persisted by callers.
// Future copies share one result, never implicitly shared QByteArray secrets.
// Taking transfers the key out; abandonment and cancellation wipe it in place.
class CredentialLookup final {
public:
    explicit CredentialLookup(std::variant<QByteArray, AgentError> value) : value_(std::move(value)) {}
    ~CredentialLookup() { wipe(); }
    std::variant<QByteArray, AgentError> take() {
        auto result = std::move(value_);
        value_ = AgentError{AgentErrorCode::Credentials, "Credential lookup result already consumed."};
        return result;
    }
    void wipe() {
        if (auto *key = std::get_if<QByteArray>(&value_)) { key->fill('\0'); key->clear(); }
    }
private:
    std::variant<QByteArray, AgentError> value_;
};
class Credentials final {
public:
    static std::variant<QByteArray, AgentError> read(const QString &reference);
    static QFuture<std::shared_ptr<CredentialLookup>> readAsync(const QString &reference);
    static std::optional<AgentError> store(const QString &reference, const QByteArray &key);
    static std::optional<AgentError> remove(const QString &reference);
};
} // namespace rose::agent
