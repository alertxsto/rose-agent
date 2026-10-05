#include "PetalDocument.h"

#include "SourceEncoding.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace rose {
namespace {
std::atomic<quint64> nextDocumentIdentity{1};

bool whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

bool delimiter(char c) {
    return whitespace(c) || c == '(' || c == ')' || c == ',' || c == '"' || c == '|';
}

ByteView unquoted(ByteView bytes) {
    return bytes.size() >= 2 && bytes.front() == '"' && bytes.back() == '"'
        ? bytes.sliced(1, bytes.size() - 2) : bytes;
}
} // namespace

WorkspaceError PetalDocument::error(ErrorCode code, QString message, qsizetype offset) const {
    WorkspaceError result{code, std::move(message), {}, offset};
    const qsizetype end = std::clamp(offset, qsizetype{0}, qsizetype(source_.size()));
    for (qsizetype i = 0; i < end; ++i) {
        if (source_.constData()[i] == '\n') {
            ++result.line;
            result.column = 1;
        } else {
            ++result.column;
        }
    }
    return result;
}

ByteView PetalDocument::raw(SourceSpan span) const {
    if (span.offset < 0 || span.length < 0 || span.offset > source_.size()
        || span.length > source_.size() - span.offset)
        throw std::out_of_range("Petal source span belongs outside document");
    return ByteView(source_).sliced(span.offset, span.length);
}

Outcome<QString> PetalDocument::decode(ByteView bytes, qsizetype offset) const {
    if (encoding_ == "ASCII") {
        for (qsizetype i = 0; i < bytes.size(); ++i) {
            if (static_cast<unsigned char>(bytes[i]) > 127)
                return error(ErrorCode::UnsupportedEncoding,
                    QStringLiteral("Non-ASCII text requires an explicit source encoding."), offset + i);
        }
        return bytes.toLatin1String();
    }
    auto decoded = sourceEncoding::decode(bytes, encoding_);
    if (const auto *failure = std::get_if<sourceEncoding::Error>(&decoded))
        return error(ErrorCode::UnsupportedEncoding,
            *failure == sourceEncoding::Error::Unsupported
                ? QStringLiteral("Unsupported source encoding: %1").arg(QString::fromLatin1(encoding_))
                : QStringLiteral("Text is invalid in the selected source encoding."), offset);
    return std::get<QString>(std::move(decoded));
}

Outcome<QString> PetalDocument::text(const PetalPart &part) const {
    if (part.child >= 0) {
        const auto &node = nodes_.at(part.child);
        if (node.parts.size() == 3 && raw(node.parts.front().span) == "value")
            return text(node.parts[2]);
        return decode(raw(part.span), part.span.offset);
    }
    const ByteView value = raw(part.span);
    if (part.kind == AtomKind::Quoted) {
        QByteArray decoded;
        decoded.reserve(std::max(qsizetype{0}, value.size() - 2));
        for (qsizetype i = 1; i + 1 < value.size(); ++i) {
            const char c = value[i];
            if (c == '\\' && i + 2 < value.size()
                && (value[i + 1] == '"' || value[i + 1] == '\\'))
                decoded.append(value[++i]);
            else
                decoded.append(c);
        }
        return decode(decoded, part.span.offset + 1);
    }
    if (part.kind == AtomKind::Text) {
        QByteArray decoded;
        decoded.reserve(value.size());
        qsizetype p = 0;
        while (p < value.size()) {
            while (p < value.size() && (value[p] == ' ' || value[p] == '\t')) ++p;
            if (p >= value.size() || value[p] != '|')
                return error(ErrorCode::InvalidSyntax, QStringLiteral("Invalid multiline text."), part.span.offset + p);
            ++p;
            while (p < value.size() && value[p] != '\r' && value[p] != '\n')
                decoded.append(value[p++]);
            if (p < value.size() && value[p] == '\r') ++p;
            if (p < value.size() && value[p] == '\n') ++p;
            if (p < value.size()) decoded.append('\n');
        }
        return decode(decoded, part.span.offset);
    }
    return decode(value, part.span.offset);
}

Outcome<PetalDocument> PetalDocument::parse(QByteArray source, QByteArray encoding) {
    PetalDocument document;
    document.source_ = std::move(source);
    document.encoding_ = std::move(encoding);
    document.identity_ = nextDocumentIdentity.fetch_add(1, std::memory_order_relaxed);
    QVector<int> stack;
    qsizetype p = 0;
    if (document.source_.startsWith("\xef\xbb\xbf")) {
        p = 3;
        if (document.encoding_ == "ASCII") document.encoding_ = "UTF-8";
    }
    if (document.encoding_ != "ASCII") {
        const auto syntax = sourceEncoding::encode(QStringLiteral("()\"\\\n ABC"), document.encoding_);
        const auto *bytes = std::get_if<QByteArray>(&syntax);
        if (!bytes || *bytes != QByteArray("()\"\\\n ABC"))
            return document.error(ErrorCode::UnsupportedEncoding,
                QStringLiteral("The source encoding must preserve Petal's ASCII syntax."), 0);
    }
    const qsizetype size = document.source_.size();
    while (p < size) {
        const char c = document.source_.constData()[p];
        if (whitespace(c)) { ++p; continue; }
        if (c == '(') {
            PetalNode node;
            node.span.offset = p;
            node.parent = stack.isEmpty() ? -1 : stack.back();
            const int index = static_cast<int>(document.nodes_.size());
            document.nodes_.append(std::move(node));
            if (stack.isEmpty()) document.roots_.append(index);
            else document.nodes_[stack.back()].parts.append({{p, 0}, AtomKind::Word, index});
            stack.append(index);
            ++p;
            continue;
        }
        if (c == ')') {
            if (stack.isEmpty())
                return document.error(ErrorCode::InvalidSyntax, QStringLiteral("Unexpected closing parenthesis."), p);
            const int index = stack.takeLast();
            auto &node = document.nodes_[index];
            node.span.length = ++p - node.span.offset;
            if (node.parent >= 0)
                document.nodes_[node.parent].parts.back().span = node.span;
            continue;
        }
        if (stack.isEmpty())
            return document.error(ErrorCode::InvalidSyntax, QStringLiteral("Expected a parenthesized Petal node."), p);
        PetalPart part;
        part.span.offset = p;
        if (c == '"') {
            part.kind = AtomKind::Quoted;
            ++p;
            bool closed = false;
            while (p < size) {
                if (document.source_.constData()[p] == '\\' && p + 1 < size
                    && (document.source_.constData()[p + 1] == '\\' || document.source_.constData()[p + 1] == '"')) {
                    p += 2;
                } else if (document.source_.constData()[p++] == '"') {
                    closed = true;
                    break;
                }
            }
            if (!closed)
                return document.error(ErrorCode::InvalidSyntax, QStringLiteral("Unterminated quoted string."), part.span.offset);
        } else if (c == '|') {
            part.kind = AtomKind::Text;
            qsizetype lineStart = p;
            while (lineStart > 0 && document.source_.constData()[lineStart - 1] != '\n') --lineStart;
            for (qsizetype i = lineStart; i < p; ++i) {
                if (document.source_.constData()[i] != ' ' && document.source_.constData()[i] != '\t' && document.source_.constData()[i] != '\r')
                    return document.error(ErrorCode::InvalidSyntax, QStringLiteral("Multiline text must begin on its own line."), p);
            }
            qsizetype lastEnd = p;
            for (;;) {
                while (p < size && document.source_.constData()[p] != '\r' && document.source_.constData()[p] != '\n') ++p;
                lastEnd = p;
                qsizetype next = p;
                if (next < size && document.source_.constData()[next] == '\r') ++next;
                if (next < size && document.source_.constData()[next] == '\n') ++next;
                while (next < size && (document.source_.constData()[next] == ' ' || document.source_.constData()[next] == '\t')) ++next;
                if (next >= size || document.source_.constData()[next] != '|') break;
                p = next;
            }
            p = lastEnd;
        } else if (c == ',') {
            part.kind = AtomKind::Comma;
            ++p;
        } else {
            part.kind = c == '@' ? AtomKind::Reference : AtomKind::Word;
            while (p < size && !delimiter(document.source_.constData()[p])) ++p;
        }
        part.span.length = p - part.span.offset;
        if (part.span.length == 0)
            return document.error(ErrorCode::InvalidSyntax, QStringLiteral("Invalid Petal token."), p);
        const auto token = document.raw(part.span);
        if (part.kind == AtomKind::Reference) {
            quint64 number = 0;
            if (token.size() < 2)
                return document.error(ErrorCode::InvalidSyntax, QStringLiteral("Reference label has no number."), part.span.offset);
            for (qsizetype i = 1; i < token.size(); ++i) {
                const char digit = token[i];
                if (digit < '0' || digit > '9' || number > (std::numeric_limits<quint64>::max() - (digit - '0')) / 10)
                    return document.error(ErrorCode::InvalidSyntax, QStringLiteral("Reference label is not an unsigned integer."), part.span.offset);
                number = number * 10 + (digit - '0');
            }
        }
        const auto nonAscii = std::find_if(token.begin(), token.end(), [](char byte) {
            return static_cast<unsigned char>(byte) > 127;
        });
        if (nonAscii != token.end()) {
            if (document.encoding_ == "ASCII")
                return document.error(ErrorCode::UnsupportedEncoding,
                    QStringLiteral("Non-ASCII text requires an explicit source encoding."),
                    part.span.offset + (nonAscii - token.begin()));
            auto decoded = document.text(part);
            if (auto *failure = std::get_if<WorkspaceError>(&decoded)) return *failure;
        }
        document.nodes_[stack.back()].parts.append(part);
    }
    if (!stack.isEmpty())
        return document.error(ErrorCode::InvalidSyntax, QStringLiteral("Unclosed Petal node."), document.nodes_[stack.back()].span.offset);
    if (document.roots_.isEmpty())
        return document.error(ErrorCode::InvalidSyntax, QStringLiteral("The document has no Petal nodes."), 0);
    auto indexed = document.buildObjectIndex();
    if (auto *failure = std::get_if<WorkspaceError>(&indexed)) return *failure;
    return document;
}

Outcome<std::monostate> PetalDocument::buildObjectIndex() {
    for (int i = 0; i < nodes_.size(); ++i) {
        auto &node = nodes_[i];
        if (node.parts.isEmpty() || node.parts[0].child >= 0 || raw(node.parts[0].span) != "object") continue;
        if (node.parts.size() < 2 || node.parts[1].child >= 0 || node.parts[1].kind != AtomKind::Word)
            return error(ErrorCode::InvalidSyntax, QStringLiteral("Petal object has no kind."), node.span.offset);
        node.headerParts = 2;
        while (node.headerParts < node.parts.size()) {
            const auto &part = node.parts[node.headerParts];
            if (part.child >= 0 || (part.kind != AtomKind::Quoted && part.kind != AtomKind::Reference)) break;
            ++node.headerParts;
        }
        for (qsizetype p = node.headerParts; p < node.parts.size(); p += 2) {
            const auto &key = node.parts[p];
            if (key.child >= 0 || key.kind != AtomKind::Word)
                return error(ErrorCode::InvalidSyntax, QStringLiteral("Expected an object property name."), key.span.offset);
            if (p + 1 == node.parts.size())
                return error(ErrorCode::InvalidSyntax, QStringLiteral("Object property has no value."), key.span.offset);
            node.properties.append({key.span, node.parts[p + 1]});
        }
        const ByteView objectKind = raw(node.parts[1].span);
        kinds_[objectKind].append(i);
        for (const auto &property : node.properties) {
            if (raw(property.key) == "quid" && property.value.child < 0 && property.value.kind == AtomKind::Quoted) {
                const auto quid = unquoted(raw(property.value.span));
                if (!identities_[objectKind].contains(quid)) identities_[objectKind].insert(quid, i);
                break;
            }
        }
    }
    return std::monostate{};
}

ByteView PetalDocument::kind(int object) const {
    const auto &node = nodes_.at(object);
    if (node.parts.size() < 2 || node.parts[0].child >= 0 || raw(node.parts[0].span) != "object") return {};
    return raw(node.parts[1].span);
}

const QVector<int> &PetalDocument::objectsOfKind(ByteView objectKind) const {
    static const QVector<int> empty;
    auto found = kinds_.constFind(objectKind);
    return found == kinds_.cend() ? empty : found.value();
}

std::optional<int> PetalDocument::objectIndex(ByteView objectKind, ByteView quid) const {
    auto kinds = identities_.constFind(objectKind);
    if (kinds == identities_.cend()) return std::nullopt;
    auto found = kinds.value().constFind(quid);
    if (found == kinds.value().cend()) return std::nullopt;
    return found.value();
}

std::optional<NameRef> PetalDocument::name(int object) const {
    const auto &node = nodes_.at(object);
    if (node.headerParts <= 2 || node.parts[2].child >= 0 || node.parts[2].kind != AtomKind::Quoted) return std::nullopt;
    const auto value = text(node.parts[2]);
    const auto *decoded = std::get_if<QString>(&value);
    if (!decoded) throw std::logic_error("Validated Petal name cannot be decoded");
    NameRef reference;
    reference.document_ = identity_;
    reference.object_ = object;
    reference.span_ = node.parts[2].span;
    reference.text_ = *decoded;
    return reference;
}

std::optional<NameRef> PetalDocument::objectName(ByteView objectKind, ByteView quid) const {
    const auto object = objectIndex(objectKind, quid);
    return object ? name(*object) : std::nullopt;
}

std::optional<PropertyRef> PetalDocument::property(int object, ByteView key, qsizetype occurrence) const {
    if (occurrence < 0) return std::nullopt;
    for (const auto &property : nodes_.at(object).properties) {
        if (raw(property.key) != key) continue;
        if (occurrence-- > 0) continue;
        PropertyRef result;
        result.document_ = identity_;
        result.span_ = property.value.span;
        return result;
    }
    return std::nullopt;
}

std::optional<PropertyRef> PetalDocument::objectProperty(ByteView objectKind, ByteView quid, ByteView key) const {
    const auto object = objectIndex(objectKind, quid);
    return object ? property(*object, key) : std::nullopt;
}

ByteView PropertyRef::rawValue(const PetalDocument &document) const {
    if (document_ != document.identity_)
        throw std::invalid_argument("Petal property belongs to another document");
    return document.raw(span_);
}

Outcome<QByteArray> PetalDocument::quote(const QString &value) const {
    QByteArray encoded;
    if (encoding_ == "ASCII") {
        for (const QChar character : value) {
            if (character.unicode() > 127)
                return error(ErrorCode::UnsupportedEncoding, QStringLiteral("The selected source encoding cannot represent this text."), 0);
        }
        encoded = value.toLatin1();
    } else {
        auto result = sourceEncoding::encode(value, encoding_);
        if (const auto *failure = std::get_if<sourceEncoding::Error>(&result))
            return error(ErrorCode::UnsupportedEncoding,
                *failure == sourceEncoding::Error::Unsupported
                    ? QStringLiteral("Unsupported source encoding.")
                    : QStringLiteral("The selected source encoding cannot represent this text."), 0);
        encoded = std::get<QByteArray>(std::move(result));
    }
    QByteArray quoted;
    quoted.reserve(encoded.size() + 2);
    quoted.append('"');
    for (char character : encoded) {
        if (character == '"' || character == '\\') quoted.append('\\');
        quoted.append(character);
    }
    quoted.append('"');
    return quoted;
}

Outcome<QByteArray> PetalDocument::replaceName(const NameRef &name, const QString &value) const {
    if (name.document_ != identity_)
        return error(ErrorCode::InvalidCommand, QStringLiteral("Name edit belongs to a different document."), 0);
    if (value.contains('\n') || value.contains('\r') || value.contains(QChar::Null))
        return error(ErrorCode::InvalidCommand, QStringLiteral("Element names cannot contain line breaks or NUL."), name.span_.offset);
    auto encoded = quote(value);
    if (auto *failure = std::get_if<WorkspaceError>(&encoded)) return *failure;
    return applyPatches({{name.span_, std::get<QByteArray>(std::move(encoded))}});
}

Outcome<QByteArray> PetalDocument::replaceValue(const PropertyRef &property, const EncodedValue &value) const {
    if (property.document_ != identity_)
        return error(ErrorCode::InvalidCommand, QStringLiteral("Property edit belongs to a different document."), 0);
    QByteArray encoded;
    if (const auto *string = std::get_if<QString>(&value)) {
        if (string->contains(QChar::Null))
            return error(ErrorCode::InvalidCommand, QStringLiteral("Property text cannot contain NUL."), property.span_.offset);
        auto quoted = quote(*string);
        if (auto *failure = std::get_if<WorkspaceError>(&quoted)) return *failure;
        encoded = std::get<QByteArray>(std::move(quoted));
    } else if (const auto *boolean = std::get_if<bool>(&value)) {
        encoded = *boolean ? "TRUE" : "FALSE";
    } else if (const auto *integer = std::get_if<qint64>(&value)) {
        encoded = QByteArray::number(*integer);
    } else if (const auto *number = std::get_if<double>(&value)) {
        if (!std::isfinite(*number))
            return error(ErrorCode::InvalidCommand, QStringLiteral("Numeric properties must be finite."), property.span_.offset);
        encoded = QByteArray::number(*number, 'g', std::numeric_limits<double>::max_digits10);
    } else {
        encoded = '@' + QByteArray::number(std::get<LocalReference>(value).value);
    }
    return applyPatches({{property.span_, std::move(encoded)}});
}

Outcome<QByteArray> PetalDocument::applyPatches(QVector<std::pair<SourceSpan, QByteArray>> patches) const {
    std::stable_sort(patches.begin(), patches.end(), [](const auto &a, const auto &b) {
        return a.first.offset < b.first.offset;
    });
    qsizetype previousEnd = 0;
    qsizetype size = source_.size();
    for (const auto &patch : patches) {
        const auto span = patch.first;
        if (span.offset < previousEnd || span.offset < 0 || span.length < 0
            || span.offset > source_.size() || span.length > source_.size() - span.offset)
            return error(ErrorCode::InvalidCommand, QStringLiteral("Source edits overlap or lie outside the document."), std::max(qsizetype{0}, span.offset));
        if (patch.second.size() > std::numeric_limits<decltype(source_.size())>::max() - (size - span.length))
            return error(ErrorCode::InvalidCommand, QStringLiteral("Edited document exceeds the addressable size."), span.offset);
        size = size - span.length + patch.second.size();
        previousEnd = span.offset + span.length;
    }
    QByteArray output;
    output.reserve(size);
    qsizetype cursor = 0;
    for (const auto &patch : patches) {
        output.append(source_.constData() + cursor, patch.first.offset - cursor);
        output.append(patch.second);
        cursor = patch.first.offset + patch.first.length;
    }
    output.append(source_.constData() + cursor, source_.size() - cursor);
    auto validated = parse(output, encoding_);
    if (auto *failure = std::get_if<WorkspaceError>(&validated)) return *failure;
    return output;
}

} // namespace rose
