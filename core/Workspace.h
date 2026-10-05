#pragma once

#include "Command.h"
#include <memory>

namespace rose {

struct RoseProfile {
    int petalVersion = 44;
    QByteArray encoding = "ASCII";
};
struct Proposal {
    ProposalId id;
    Revision baseRevision = 0;
    QByteArray digest;
    QVector<DiffEntry> diff;
    QVector<Diagnostic> diagnostics;
    QMap<QString, QString> newIds;
};
struct Approval {
    ProposalId id;
    Revision baseRevision = 0;
    QByteArray digest;
};
struct Applied {
    Revision revision = 0;
    QString transactionId;
    QMap<QString, QString> newIds;
};
struct SaveMode { QString destination; };
struct SaveReceipt {
    Revision revision = 0;
    QMap<UnitId, QByteArray> checksums;
    QString path;
};
class Workspace {
public:
    static Outcome<std::unique_ptr<Workspace>> create(const QString &, RoseProfile, const AccessPolicy &);
    static Outcome<std::unique_ptr<Workspace>> open(const QString &, const AccessPolicy &);
    ~Workspace();
    Workspace(const Workspace &) = delete;
    Workspace &operator=(const Workspace &) = delete;
    Outcome<Projection> inspect(const Query &) const;
    Outcome<Proposal> propose(Revision base, const QVector<Command> &);
    Outcome<Applied> apply(const Approval &);
    Status reject(const ProposalId &);
    Outcome<Applied> undo();
    Outcome<Applied> redo();
    Outcome<SaveReceipt> save(const SaveMode & = {});
    Status grantDirectory(const QString &);
    Revision revision() const;
    bool dirty() const;
    QString path() const;
private:
    struct Impl;
    explicit Workspace(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

struct Closed {};
struct Rejected { ProposalId id; };
struct AccessGranted { QString directory; };
using WorkspaceReply = std::variant<Projection, Proposal, Applied, SaveReceipt, Closed, Rejected, AccessGranted, WorkspaceError>;

} // namespace rose

Q_DECLARE_METATYPE(rose::WorkspaceReply)
