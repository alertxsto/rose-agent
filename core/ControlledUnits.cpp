#include "ControlledUnits.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include "SourceEncoding.h"
#include <utility>

namespace rose {
namespace {
WorkspaceError failure(ErrorCode code, QString message, QString path = {}) {
    return {code, std::move(message), std::move(path)};
}
QString diagnosticCode(ErrorCode code) {
    switch (code) {
    case ErrorCode::InvalidSyntax: return "InvalidSyntax";
    case ErrorCode::UnsupportedProfile: return "UnsupportedProfile";
    case ErrorCode::UnsupportedEncoding: return "UnsupportedEncoding";
    case ErrorCode::MissingUnit: return "MissingUnit";
    case ErrorCode::AccessDenied: return "AccessDenied";
    case ErrorCode::UnsafeRewrite: return "UnsafeRewrite";
    default: return "StorageFailure";
    }
}
bool nativeSuffix(const QString &path) {
    const auto suffix = QFileInfo(path).suffix().toLower();
    return suffix == "mdl" || suffix == "cat" || suffix == "sub" || suffix == "prc" || suffix == "prp";
}
struct DependencyField {
    SourceSpan span;
    QString spelling;
    bool known = false;
};
// Only explicit native unit fields authorize model reads. An arbitrary vendor
// file_name is preserved and surfaced, but is not authority to fetch a resource.
Outcome<QVector<DependencyField>> dependencyFields(const PetalDocument &document) {
    QVector<DependencyField> result;
    const auto &nodes = document.nodes();
    for (qsizetype i = 0; i < nodes.size(); ++i) {
        if (nodes[i].parts.isEmpty() || document.raw(nodes[i].parts.front().span) != "object") continue;
        qsizetype unitFlags = 0, fileFields = 0;
        bool unit = false;
        for (const auto &property : nodes[i].properties) {
            const auto key = document.raw(property.key);
            if (key == "is_unit") {
                ++unitFlags;
                unit = property.value.child < 0 && document.raw(property.value.span) == "TRUE";
            }
            if (key == "file_name") ++fileFields;
        }
        for (const auto &property : nodes[i].properties) {
            if (document.raw(property.key) != "file_name") continue;
            auto spelling = document.text(property.value);
            if (auto e = std::get_if<WorkspaceError>(&spelling)) return *e;
            const auto path = std::get<QString>(std::move(spelling));
            const bool scalar = property.value.child < 0 && property.value.kind == AtomKind::Quoted;
            result.append({property.value.span, path,
                           scalar && unit && unitFlags == 1 && fileFields == 1});
        }
    }
    return result;
}
// Match PetalDocument's explicit-codec quoted strings while batching patches.
Outcome<QByteArray> quotedPath(const QString &path, const PetalDocument &document, SourceSpan span) {
    QByteArray encoded;
    if (document.encoding() == "ASCII") {
        for (const auto character : path) if (character.unicode() > 127)
            return document.error(ErrorCode::UnsupportedEncoding, "Dependency path cannot be represented in the source encoding", span.offset);
        encoded = path.toLatin1();
    } else {
        auto result = sourceEncoding::encode(path, document.encoding());
        if (const auto *error = std::get_if<sourceEncoding::Error>(&result))
            return document.error(ErrorCode::UnsupportedEncoding, *error == sourceEncoding::Error::Unsupported
                ? "Unsupported source encoding" : "Dependency path cannot be represented in the source encoding", span.offset);
        encoded = std::get<QByteArray>(std::move(result));
    }
    QByteArray quoted;
    quoted.reserve(encoded.size() + 2);
    quoted.append('"');
    for (const auto character : encoded) {
        if (character == '"' || character == '\\') quoted.append('\\');
        quoted.append(character);
    }
    quoted.append('"');
    return quoted;
}
QString pathKey(const QString &path) {
#ifdef Q_OS_WIN
    return path.toCaseFolded();
#else
    return path;
#endif
}
}

Outcome<QString> ControlledUnits::resolvePath(const QString &spelling, const QString &sourcePath,
                                             const AccessPolicy &input, bool allowMissing) {
    auto normalized = storage::canonicalPolicy(input);
    if (auto e = std::get_if<WorkspaceError>(&normalized)) return *e;
    const auto &policy = std::get<AccessPolicy>(normalized);
    if (spelling.isEmpty() || spelling.contains(QChar::Null))
        return failure(ErrorCode::AccessDenied, "Empty or invalid dependency path", sourcePath);
    static const QRegularExpression variable("\\$(?:([A-Za-z_][A-Za-z0-9_]*)|\\{([A-Za-z_][A-Za-z0-9_]*)\\})");
    QString expanded;
    qsizetype cursor = 0;
    auto matches = variable.globalMatch(spelling);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const auto literal = spelling.mid(cursor, match.capturedStart() - cursor);
        if (literal.contains('$')) return failure(ErrorCode::MissingUnit, "Invalid explicit path variable", sourcePath);
        expanded += literal;
        const auto name = match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
        const auto value = policy.pathVariables.constFind(name);
        if (value == policy.pathVariables.cend())
            return failure(ErrorCode::MissingUnit, "Unknown explicit path variable: " + name, sourcePath);
        expanded += value.value();
        cursor = match.capturedEnd();
    }
    const auto remainder = spelling.mid(cursor);
    if (remainder.contains('$')) return failure(ErrorCode::MissingUnit, "Invalid explicit path variable", sourcePath);
    expanded += remainder;
    expanded.replace('\\', '/');
    if (expanded.contains("://") || expanded.startsWith("//"))
        return failure(ErrorCode::AccessDenied, "Network dependency paths are not authorized", sourcePath);
#ifndef Q_OS_WIN
    if (expanded.size() >= 2 && expanded[0].isLetter() && expanded[1] == ':')
        return failure(ErrorCode::AccessDenied, "Windows drive paths cannot be resolved on this host", sourcePath);
#endif
    const auto path = QDir::isAbsolutePath(expanded) ? expanded : QDir(QFileInfo(sourcePath).absolutePath()).filePath(expanded);
    // A missing path behind an escaping symlink is still unauthorized.
    auto ancestor = QFileInfo(path).absolutePath();
    while (!QFileInfo(ancestor).exists() && !QFileInfo(ancestor).isSymLink()) {
        const auto parent = QFileInfo(ancestor).absolutePath();
        if (parent == ancestor) break;
        ancestor = parent;
    }
    auto approvedAncestor = storage::canonicalPath(ancestor, policy);
    if (auto e = std::get_if<WorkspaceError>(&approvedAncestor)) return *e;
    return storage::canonicalPath(path, policy, allowMissing);
}

Outcome<LoadedUnits> ControlledUnits::load(const QString &rootPath, const AccessPolicy &input) {
    auto normalized = storage::canonicalPolicy(input);
    if (auto e = std::get_if<WorkspaceError>(&normalized)) return *e;
    const auto &policy = std::get<AccessPolicy>(normalized);
    auto rootRead = storage::read(rootPath, policy);
    if (auto e = std::get_if<WorkspaceError>(&rootRead)) return *e;
    auto root = std::get<storage::Snapshot>(std::move(rootRead));
    auto parsed = PetalDocument::parse(std::move(root.bytes), policy.sourceEncoding);
    if (auto e = std::get_if<WorkspaceError>(&parsed)) { e->file = root.path; return *e; }
    LoadedUnits loaded;
    const UnitId rootId{root.path};
    loaded.documents.append({rootId, root.path, std::get<PetalDocument>(std::move(parsed)), policy.writable});
    loaded.summaries.append({rootId, root.path, policy.writable, true});
    loaded.checksums.insert(rootId, root.checksum);
    struct Frame { qsizetype document = 0, next = 0; QVector<DependencyField> fields; };
    auto rootFields = dependencyFields(loaded.documents.front().document);
    if (auto e = std::get_if<WorkspaceError>(&rootFields)) return *e;
    QVector<Frame> stack;
    stack.append({0, 0, std::get<QVector<DependencyField>>(std::move(rootFields))});
    QMap<UnitId, int> state; // 1 = active, 2 = fully visited, 3 = unresolved
    QMap<UnitId, qsizetype> summaries;
    state.insert(rootId, 1); summaries.insert(rootId, 0);
    auto unresolved = [&](const UnitReference &reference, const WorkspaceError &error) {
        const auto id = reference.target.isEmpty() ? UnitId{reference.spelling} : reference.target;
        if (!summaries.contains(id)) {
            summaries.insert(id, loaded.summaries.size());
            loaded.summaries.append({id, reference.path.isEmpty() ? reference.spelling : reference.path, false, false});
        }
        loaded.diagnostics.append({Severity::Warning, diagnosticCode(error.code), error.message,
                                   reference.source.value, {}, reference.span});
    };
    while (!stack.isEmpty()) {
        auto &frame = stack.back();
        const auto sourceId = loaded.documents[frame.document].id;
        const auto sourcePath = loaded.documents[frame.document].path;
        if (frame.next == frame.fields.size()) {
            state[sourceId] = 2;
            stack.removeLast();
            continue;
        }
        const auto field = frame.fields[frame.next++];
        UnitReference reference{sourceId, {}, field.spelling, {}, field.span, false, field.known, false};
        if (!field.known) {
            unresolved(reference, failure(ErrorCode::UnsafeRewrite, "Unknown dependency field is preserved but cannot be loaded or relocated", sourcePath));
            loaded.references.append(std::move(reference));
            continue;
        }
        auto resolved = resolvePath(field.spelling, sourcePath, policy, true);
        if (auto e = std::get_if<WorkspaceError>(&resolved)) {
            unresolved(reference, *e); loaded.references.append(std::move(reference)); continue;
        }
        reference.path = std::get<QString>(std::move(resolved));
        reference.target = UnitId{reference.path};
        if (reference.target != rootId && !nativeSuffix(reference.path)) {
            reference.model = false;
            unresolved(reference, failure(ErrorCode::UnsafeRewrite, "Native unit has an unsupported dependency extension; resource was not read", sourcePath));
            loaded.references.append(std::move(reference));
            continue;
        }
        reference.referenceModel = QFileInfo(reference.path).suffix().compare("mdl", Qt::CaseInsensitive) == 0 && reference.target != rootId;
        const auto visit = state.value(reference.target);
        if (visit == 1 || visit == 2) {
            reference.resolved = true;
            if (visit == 1) loaded.diagnostics.append({Severity::Warning, "UnitCycle", "Controlled-unit reference closes a cycle", sourcePath, {}, field.span});
            loaded.references.append(std::move(reference));
            continue;
        }
        if (visit == 3) { loaded.references.append(std::move(reference)); continue; }
        auto read = storage::read(reference.path, policy);
        if (auto e = std::get_if<WorkspaceError>(&read)) {
            state[reference.target] = 3; unresolved(reference, *e); loaded.references.append(std::move(reference)); continue;
        }
        auto snapshot = std::get<storage::Snapshot>(std::move(read));
        auto document = PetalDocument::parse(std::move(snapshot.bytes), policy.sourceEncoding);
        if (auto e = std::get_if<WorkspaceError>(&document)) {
            state[reference.target] = 3; unresolved(reference, *e); loaded.references.append(std::move(reference)); continue;
        }
        auto fields = dependencyFields(std::get<PetalDocument>(document));
        if (auto e = std::get_if<WorkspaceError>(&fields)) {
            state[reference.target] = 3; unresolved(reference, *e); loaded.references.append(std::move(reference)); continue;
        }
        reference.resolved = true;
        const auto id = reference.target;
        state[id] = 1;
        summaries.insert(id, loaded.summaries.size());
        loaded.summaries.append({id, snapshot.path, policy.writable, true});
        loaded.checksums.insert(id, snapshot.checksum);
        const auto index = loaded.documents.size();
        loaded.documents.append({id, snapshot.path, std::get<PetalDocument>(std::move(document)), policy.writable});
        loaded.references.append(std::move(reference));
        stack.append({index, 0, std::get<QVector<DependencyField>>(std::move(fields))});
    }
    // Reference models own a read-only dependency closure, except the source
    // root: a back edge must not downgrade the user's explicitly opened model.
    QMap<UnitId, QVector<UnitId>> dependencies;
    QVector<UnitId> pending;
    for (const auto &reference : loaded.references) if (reference.resolved) {
        dependencies[reference.source].append(reference.target);
        if (reference.referenceModel) pending.append(reference.target);
    }
    QSet<UnitId> readOnly;
    while (!pending.isEmpty()) {
        const auto id = pending.takeLast();
        if (id == rootId || readOnly.contains(id)) continue;
        readOnly.insert(id);
        const auto children = dependencies.constFind(id);
        if (children != dependencies.cend()) for (const auto &dependency : children.value()) pending.append(dependency);
    }
    for (auto &document : loaded.documents) if (readOnly.contains(document.id)) document.writable = false;
    for (auto &summary : loaded.summaries) if (readOnly.contains(summary.id)) summary.writable = false;
    return loaded;
}

Outcome<QVector<storage::Write>> ControlledUnits::relocate(
    const LoadedUnits &loaded, const QMap<UnitId, QByteArray> &serialized,
    const QString &oldRoot, const QString &newRoot, const AccessPolicy &input) {
    auto normalized = storage::canonicalPolicy(input);
    if (auto e = std::get_if<WorkspaceError>(&normalized)) return *e;
    const auto &policy = std::get<AccessPolicy>(normalized);
    if (!policy.writable) return failure(ErrorCode::AccessDenied, "Relocation requires explicit write access", newRoot);
    auto oldPath = storage::canonicalPath(oldRoot, policy, true);
    if (auto e = std::get_if<WorkspaceError>(&oldPath)) return *e;
    auto newPath = storage::canonicalPath(newRoot, policy, true);
    if (auto e = std::get_if<WorkspaceError>(&newPath)) return *e;
    const auto sourceRoot = std::get<QString>(oldPath), destinationRoot = std::get<QString>(newPath);
    const QDir sourceDirectory(QFileInfo(sourceRoot).absolutePath()), destinationDirectory(QFileInfo(destinationRoot).absolutePath());
    for (const auto &reference : loaded.references) if (!reference.resolved)
        return failure(reference.model ? ErrorCode::MissingUnit : ErrorCode::UnsafeRewrite,
                       "Unresolved dependency prevents relocation", reference.spelling);
    QMap<QString, QString> destinations;
    QSet<QString> targets;
    auto addTarget = [&](const QString &source) -> Status {
        const auto relative = sourceDirectory.relativeFilePath(source);
        if (relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative))
            return failure(ErrorCode::UnsafeRewrite, "Dependency outside the source-root directory cannot retain its layout under the new root", source);
        const auto proposed = source == sourceRoot ? destinationRoot : destinationDirectory.filePath(relative);
        auto canonical = storage::canonicalPath(proposed, policy, true);
        if (auto e = std::get_if<WorkspaceError>(&canonical)) return *e;
        const auto target = std::get<QString>(std::move(canonical));
        const auto targetRelative = destinationDirectory.relativeFilePath(target);
        if (targetRelative == ".." || targetRelative.startsWith("../") || QDir::isAbsolutePath(targetRelative))
            return failure(ErrorCode::AccessDenied, "Relocated dependency escapes the destination-root directory", target);
        if (destinations.contains(source) || targets.contains(pathKey(target)))
            return failure(ErrorCode::UnsafeRewrite, "Relocated files collide at the destination", target);
        auto existing = storage::read(target, policy, true);
        if (auto e = std::get_if<WorkspaceError>(&existing)) return *e;
        if (std::get<storage::Snapshot>(existing).exists)
            return failure(ErrorCode::DiskConflict, "Relocation destination already exists; no overwrite performed", target);
        destinations.insert(source, target); targets.insert(pathKey(target));
        return std::monostate{};
    };
    bool hasRoot = false;
    for (const auto &document : loaded.documents) {
        if (document.path == sourceRoot) hasRoot = true;
        if (!serialized.contains(document.id)) return failure(ErrorCode::UnsafeRewrite, "Serializer omitted a loaded unit", document.path);
        if (!document.writable && serialized.value(document.id) != document.document.bytes())
            return failure(ErrorCode::AccessDenied, "Reference model cannot be relocated with mutations", document.path);
        if (loaded.checksums.contains(document.id)) {
            auto current = storage::read(document.path, policy);
            if (auto e = std::get_if<WorkspaceError>(&current)) return *e;
            if (std::get<storage::Snapshot>(current).checksum != loaded.checksums.value(document.id))
                return failure(ErrorCode::DiskConflict, "Source unit changed before relocation", document.path);
        }
        auto target = addTarget(document.path); if (auto e = std::get_if<WorkspaceError>(&target)) return *e;
    }
    if (!hasRoot) return failure(ErrorCode::UnsafeRewrite, "Source root is not present in the loaded document set", sourceRoot);
    for (const auto &artifact : loaded.artifacts) {
        auto current = storage::read(artifact.path, policy);
        if (auto e = std::get_if<WorkspaceError>(&current)) return *e;
        if (std::get<storage::Snapshot>(current).checksum != artifact.checksum || storage::checksum(artifact.bytes) != artifact.checksum)
            return failure(ErrorCode::DiskConflict, "Passive dependency changed before relocation", artifact.path);
        auto target = addTarget(artifact.path); if (auto e = std::get_if<WorkspaceError>(&target)) return *e;
    }
    QVector<storage::Write> writes;
    for (const auto &unit : loaded.documents) {
        auto parsed = PetalDocument::parse(serialized.value(unit.id), unit.document.encoding());
        if (auto e = std::get_if<WorkspaceError>(&parsed)) { e->file = unit.path; return *e; }
        auto document = std::get<PetalDocument>(std::move(parsed));
        auto scanned = dependencyFields(document);
        if (auto e = std::get_if<WorkspaceError>(&scanned)) return *e;
        const auto &fields = std::get<QVector<DependencyField>>(scanned);
        // Discover current spans: preceding serialized names may have changed.
        QVector<std::pair<SourceSpan, QByteArray>> patches;
        patches.reserve(fields.size());
        const QDir referringDirectory(QFileInfo(destinations.value(unit.path)).absolutePath());
        for (const auto &field : fields) {
            if (!field.known) return failure(ErrorCode::UnsafeRewrite, "Unknown dependency field cannot be relocated", unit.path);
            auto resolved = resolvePath(field.spelling, unit.path, policy);
            if (auto e = std::get_if<WorkspaceError>(&resolved)) return *e;
            const auto target = destinations.constFind(std::get<QString>(resolved));
            if (target == destinations.cend()) return failure(ErrorCode::MissingUnit, "Serialized dependency was not loaded and verified", field.spelling);
            auto unchanged = resolvePath(field.spelling, destinations.value(unit.path), policy, true);
            if (auto path = std::get_if<QString>(&unchanged); path && *path == target.value()) continue;
            const auto spelling = referringDirectory.relativeFilePath(target.value());
            if (spelling == field.spelling) continue;
            auto quoted = quotedPath(spelling, document, field.span);
            if (auto e = std::get_if<WorkspaceError>(&quoted)) return *e;
            patches.append({field.span, std::get<QByteArray>(std::move(quoted))});
        }
        auto rewritten = document.applyPatches(std::move(patches));
        if (auto e = std::get_if<WorkspaceError>(&rewritten)) return *e;
        writes.append({destinations.value(unit.path), std::get<QByteArray>(std::move(rewritten)), std::nullopt});
    }
    for (const auto &artifact : loaded.artifacts) writes.append({destinations.value(artifact.path), artifact.bytes, std::nullopt});
    return writes;
}

} // namespace rose
