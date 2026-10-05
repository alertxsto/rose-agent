#pragma once

#include "Types.h"
#include "ByteView.h"
#include <QHash>
#include <QVector>
#include <optional>

namespace rose {

class PetalDocument;

enum class AtomKind { Word, Quoted, Text, Reference, Comma };

struct PetalPart {
    SourceSpan span;
    AtomKind kind = AtomKind::Word;
    int child = -1;
};

struct PetalProperty {
    SourceSpan key;
    PetalPart value;
};

struct PetalNode {
    SourceSpan span;
    int parent = -1;
    QVector<PetalPart> parts;
    QVector<PetalProperty> properties;
    qsizetype headerParts = 0;
};

class NameRef {
public:
    const QString &text() const noexcept { return text_; }
    int objectIndex() const noexcept { return object_; }
private:
    friend class PetalDocument;
    quint64 document_ = 0;
    int object_ = -1;
    SourceSpan span_;
    QString text_;
};

class PropertyRef {
public:
    ByteView rawValue(const PetalDocument &document) const;
    SourceSpan span() const noexcept { return span_; }
private:
    friend class PetalDocument;
    quint64 document_ = 0;
    SourceSpan span_;
};

struct LocalReference { quint64 value; };
using EncodedValue = std::variant<QString, bool, qint64, double, LocalReference>;

class PetalDocument {
public:
    // Without an explicit legacy codec, non-ASCII input is rejected rather
    // than guessed. Unchanged source bytes are never normalized.
    static Outcome<PetalDocument> parse(QByteArray source, QByteArray encoding = "ASCII");
    const QByteArray &bytes() const noexcept { return source_; }
    const QByteArray &encoding() const noexcept { return encoding_; }
    const QVector<PetalNode> &nodes() const noexcept { return nodes_; }
    const QVector<int> &roots() const noexcept { return roots_; }
    ByteView raw(SourceSpan span) const;
    ByteView kind(int object) const;
    const QVector<int> &objectsOfKind(ByteView kind) const;
    std::optional<int> objectIndex(ByteView kind, ByteView quid) const;
    std::optional<NameRef> objectName(ByteView kind, ByteView quid) const;
    std::optional<NameRef> name(int object) const;
    std::optional<PropertyRef> objectProperty(ByteView kind, ByteView quid, ByteView key) const;
    std::optional<PropertyRef> property(int object, ByteView key, qsizetype occurrence = 0) const;
    Outcome<QString> text(const PetalPart &part) const;
    Outcome<QByteArray> replaceName(const NameRef &name, const QString &value) const;
    Outcome<QByteArray> replaceValue(const PropertyRef &property, const EncodedValue &value) const;
    Outcome<QByteArray> applyPatches(QVector<std::pair<SourceSpan, QByteArray>> patches) const;
    WorkspaceError error(ErrorCode code, QString message, qsizetype offset) const;

private:
    friend class PropertyRef;
    Outcome<QString> decode(ByteView bytes, qsizetype offset) const;
    Outcome<QByteArray> quote(const QString &value) const;
    Outcome<std::monostate> buildObjectIndex();
    QByteArray source_;
    QByteArray encoding_;
    quint64 identity_ = 0;
    QVector<PetalNode> nodes_;
    QVector<int> roots_;
    QHash<ByteView, QVector<int>> kinds_;
    QHash<ByteView, QHash<ByteView, int>> identities_;
};

} // namespace rose
