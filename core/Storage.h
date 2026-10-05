#pragma once

#include "Types.h"
#include "ByteView.h"

namespace rose::storage {

struct Snapshot {
    QString path;
    QByteArray bytes, checksum;
    bool exists = false;
};
struct Write {
    QString path;
    QByteArray bytes;
    // No checksum means that the destination must not exist.
    std::optional<QByteArray> expectedChecksum;
};

QByteArray checksum(ByteView bytes);
Outcome<AccessPolicy> canonicalPolicy(const AccessPolicy &);
Outcome<QString> canonicalPath(const QString &, const AccessPolicy &, bool allowMissing = false);
Outcome<Snapshot> read(const QString &, const AccessPolicy &, bool allowMissing = false);
QString journalPath(const QString &modelPath);
// A pending journal is rolled back; a durably committed journal is verified
// and cleaned. Externally changed targets/backups are never overwritten.
Status recover(const QString &modelPath, const AccessPolicy &);
Outcome<QMap<QString, QByteArray>> save(const QVector<Write> &, const QString &modelPath,
                                      const AccessPolicy &);

} // namespace rose::storage
