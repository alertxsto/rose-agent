#pragma once

#include "Model.h"
#include "Storage.h"

namespace rose {

struct UnitReference {
    UnitId source, target;
    QString spelling, path;
    SourceSpan span;
    bool resolved = false, model = true, referenceModel = false;
};
struct PassiveArtifact {
    QString path;
    QByteArray bytes, checksum;
};
struct LoadedUnits {
    QVector<UnitDocument> documents;
    QVector<UnitSummary> summaries;
    QVector<UnitReference> references;
    QVector<PassiveArtifact> artifacts;
    QVector<Diagnostic> diagnostics;
    QMap<UnitId, QByteArray> checksums;
};
class ControlledUnits {
public:
    static Outcome<QString> resolvePath(const QString &spelling, const QString &sourcePath,
                                        const AccessPolicy &, bool allowMissing = false);
    static Outcome<LoadedUnits> load(const QString &rootPath, const AccessPolicy &);
    // Relocates dependency fields, copies passive dependencies and refuses
    // unresolved dependencies. Destination files are never silently overwritten.
    static Outcome<QVector<storage::Write>> relocate(
        const LoadedUnits &, const QMap<UnitId, QByteArray> &serialized,
        const QString &oldRoot, const QString &newRoot, const AccessPolicy &);
};

} // namespace rose
