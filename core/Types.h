#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QMap>
#include <QMetaType>
#include <QVector>
#include <optional>
#include <variant>

namespace rose {

enum class ErrorCode {
    InvalidSyntax, UnsupportedProfile, UnsupportedEncoding, MissingUnit,
    AccessDenied, InvalidCommand, DanglingReference, StaleRevision,
    StaleApproval, DiskConflict, UnsafeRewrite, StorageFailure
};

struct WorkspaceError {
    ErrorCode code;
    QString message;
    QString file;
    qsizetype offset = 0;
    qsizetype line = 1;
    qsizetype column = 1;
};

template<class T> using Outcome = std::variant<T, WorkspaceError>;
using Status = Outcome<std::monostate>;
using Revision = quint64;

struct SourceSpan {
    qsizetype offset = 0;
    qsizetype length = 0;
};

template<class Tag> struct Id {
    QString value;
    bool isEmpty() const noexcept { return value.isEmpty(); }
    friend bool operator==(const Id &, const Id &) = default;
    friend bool operator<(const Id &a, const Id &b) noexcept { return a.value < b.value; }
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    friend uint qHash(const Id &id, uint seed = 0) noexcept { return ::qHash(id.value, seed); }
#else
    friend size_t qHash(const Id &id, size_t seed = 0) noexcept { return ::qHash(id.value, seed); }
#endif
};
using ElementId = Id<struct ElementTag>;
using RelationId = Id<struct RelationTag>;
using DiagramId = Id<struct DiagramTag>;
using PresentationId = Id<struct PresentationTag>;
using UnitId = Id<struct UnitTag>;
using ProposalId = Id<struct ProposalTag>;

struct WorldPoint {
    double x = 0, y = 0;
    friend bool operator==(const WorldPoint &, const WorldPoint &) = default;
};
struct Geometry {
    double x = 0, y = 0, width = 160, height = 100;
    friend bool operator==(const Geometry &, const Geometry &) = default;
};
struct AccessPolicy {
    QStringList allowedDirectories;
    QMap<QString, QString> pathVariables;
    QByteArray sourceEncoding = "ASCII";
    bool writable = true;
};
enum class Severity { Information, Warning, Error };
struct Diagnostic {
    Severity severity = Severity::Warning;
    QString code, message, file;
    QString objectId;
    SourceSpan span;
};

} // namespace rose
