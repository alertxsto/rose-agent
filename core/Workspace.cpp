#include "Workspace.h"
#include "ControlledUnits.h"
#include "Json.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QUuid>
#include <QSet>
#include <limits>

namespace rose {
namespace {
WorkspaceError error(ErrorCode code, QString message, QString path = {}) { return {code, std::move(message), std::move(path)}; }
QString uid() {
    // Rose's documented shape: time seconds followed by a random suffix.
    return QString::number(QDateTime::currentSecsSinceEpoch(), 16).rightJustified(8, '0').toUpper()
        + QUuid::createUuid().toString(QUuid::Id128).left(4).toUpper();
}
QByteArray freshModel(int version) {
    QSet<QString> allocated;
    auto nextId = [&]() {
        QString id;
        do { id = uid(); } while (allocated.contains(id));
        allocated.insert(id); return id;
    };
    const auto design = nextId(), logical = nextId(), usecase = nextId(), component = nextId(), processes = nextId();
    auto diagram = [&](const QString &kind, const QString &name) {
        return QString("(object %1 \"%2\" quid \"%3\" title \"%2\" zoom 100 max_height 28350 max_width 21600 origin_x 0 origin_y 0 items (list diagram_item_list))").arg(kind, name, nextId());
    };
    return QString(
        "(object Petal version %11 _written \"Rose Agent authored class profile\" charSet 0)\n"
        "(object Design \"Logical View\" is_unit TRUE is_loaded TRUE quid \"%1\"%12\n"
        " defaults (object defaults rightMargin 0.250000 leftMargin 0.250000 topMargin 0.250000 bottomMargin 0.500000 pageOverlap 0.250000 clipIconLabels TRUE autoResize TRUE snapToGrid TRUE gridX 3 gridY 3 defaultFont (object Font size 10 face \"Arial\" bold FALSE italics FALSE underline FALSE strike FALSE color 0 default_color TRUE) showMessageNum 3 showClassOfObject TRUE notation \"Unified\")\n"
        " root_usecase_package (object Class_Category \"Use Case View\" quid \"%2\" exportControl \"Public\" global TRUE logical_models (list unit_reference_list) logical_presentations (list unit_reference_list %6))\n"
        " root_category (object Class_Category \"Logical View\" quid \"%3\" exportControl \"Public\" global TRUE subsystem \"Component View\" quidu \"%4\" logical_models (list unit_reference_list) logical_presentations (list unit_reference_list %7))\n"
        " root_subsystem (object SubSystem \"Component View\" quid \"%4\" physical_models (list unit_reference_list) physical_presentations (list unit_reference_list %8)%13)\n"
        " process_structure (object Processes quid \"%5\" ProcsNDevs (list %9))\n"
        " properties (object Properties%10 attributes (list Attribute_Set)))\n")
        .arg(design, usecase, logical, component, processes,
             diagram("UseCaseDiagram", "Main"), diagram("ClassDiagram", "Main"),
             diagram("Module_Diagram", "Main"), diagram("Process_Diagram", "Deployment View"))
        .arg(version == 50 ? QString(" quid \"%1\"").arg(nextId()) : QString{},
             QString::number(version), version == 50 ? " enforceClosureAutoLoad FALSE" : "",
             version == 50 ? QString(" category \"Logical View\" quidu \"%1\"").arg(logical) : QString{}).toLatin1();
}
}
struct Workspace::Impl {
    struct Pending { Proposal proposal; ModelDelta delta; };
    struct History { QString id; ModelDelta delta; };
    QString path;
    AccessPolicy policy;
    LoadedUnits units;
    std::unique_ptr<Model> model;
    Revision revision = 0;
    bool dirty = false;
    QMap<ProposalId, Pending> proposals;
    QVector<History> undo, redo;
    Status nextRevision() const {
        if (revision == std::numeric_limits<Revision>::max()) return error(ErrorCode::InvalidCommand, "Revision space exhausted");
        return std::monostate{};
    }
    void changed() { ++revision; dirty = true; proposals.clear(); }
};
Workspace::Workspace(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Workspace::~Workspace() = default;
Outcome<std::unique_ptr<Workspace>> Workspace::open(const QString &path, const AccessPolicy &input) {
    auto policy = storage::canonicalPolicy(input); if (auto e = std::get_if<WorkspaceError>(&policy)) return *e;
    auto canonical = storage::canonicalPath(path, std::get<AccessPolicy>(policy)); if (auto e = std::get_if<WorkspaceError>(&canonical)) return *e;
    auto recovered = storage::recover(std::get<QString>(canonical), std::get<AccessPolicy>(policy)); if (auto e = std::get_if<WorkspaceError>(&recovered)) return *e;
    auto loaded = ControlledUnits::load(std::get<QString>(canonical), std::get<AccessPolicy>(policy)); if (auto e = std::get_if<WorkspaceError>(&loaded)) return *e;
    auto impl = std::make_unique<Impl>(); impl->path = std::get<QString>(canonical); impl->policy = std::get<AccessPolicy>(policy);
    impl->units = std::move(std::get<LoadedUnits>(loaded));
    auto model = Model::fromDocuments(impl->units.documents); if (auto e = std::get_if<WorkspaceError>(&model)) return *e;
    impl->model = std::move(std::get<std::unique_ptr<Model>>(model));
    return std::unique_ptr<Workspace>(new Workspace(std::move(impl)));
}
Outcome<std::unique_ptr<Workspace>> Workspace::create(const QString &path, RoseProfile profile, const AccessPolicy &input) {
    if ((profile.petalVersion != 44 && profile.petalVersion != 50) || profile.encoding != "ASCII") return error(ErrorCode::UnsupportedProfile, "New models require an authored Petal44 or Petal50 ASCII class profile", path);
    auto policy = storage::canonicalPolicy(input); if (auto e = std::get_if<WorkspaceError>(&policy)) return *e;
    if (!std::get<AccessPolicy>(policy).writable) return error(ErrorCode::AccessDenied, "Creating a model requires write access", path);
    auto destination = storage::read(path, std::get<AccessPolicy>(policy), true); if (auto e = std::get_if<WorkspaceError>(&destination)) return *e;
    const auto &snapshot = std::get<storage::Snapshot>(destination);
    if (snapshot.exists) return error(ErrorCode::DiskConflict, "New model destination already exists", path);
    auto parsed = PetalDocument::parse(freshModel(profile.petalVersion)); if (auto e = std::get_if<WorkspaceError>(&parsed)) return *e;
    auto impl = std::make_unique<Impl>(); impl->path = snapshot.path; impl->policy = std::get<AccessPolicy>(policy); impl->dirty = true;
    const UnitId id{snapshot.path};
    impl->units.documents.append({id, snapshot.path, std::move(std::get<PetalDocument>(parsed)), true});
    impl->units.summaries.append({id, snapshot.path, true, true});
    impl->units.diagnostics.append({Severity::Warning, "NativeOracleGap", QString("Authored Petal%1 class profile; native Rose open/resave is an independent acceptance gate").arg(profile.petalVersion), snapshot.path, {}, {}});
    auto model = Model::fromDocuments(impl->units.documents); if (auto e = std::get_if<WorkspaceError>(&model)) return *e;
    impl->model = std::move(std::get<std::unique_ptr<Model>>(model));
    return std::unique_ptr<Workspace>(new Workspace(std::move(impl)));
}
Outcome<Projection> Workspace::inspect(const Query &query) const {
    auto inspected = impl_->model->inspect(query, impl_->revision); if (auto e = std::get_if<WorkspaceError>(&inspected)) return *e;
    auto projection = std::move(std::get<Projection>(inspected));
    projection.path = impl_->path; projection.dirty = impl_->dirty; projection.canUndo = !impl_->undo.isEmpty(); projection.canRedo = !impl_->redo.isEmpty();
    projection.units = impl_->units.summaries; projection.diagnostics += impl_->units.diagnostics;
    return projection;
}
Outcome<Proposal> Workspace::propose(Revision base, const QVector<Command> &commands) {
    if (base != impl_->revision) return error(ErrorCode::StaleRevision, "Proposal base revision is no longer current");
    if (!impl_->policy.writable) return error(ErrorCode::AccessDenied, "Model is read-only", impl_->path);
    auto staged = impl_->model->stage(commands); if (auto e = std::get_if<WorkspaceError>(&staged)) return *e;
    auto delta = std::move(std::get<ModelDelta>(staged));
    if (delta.isEmpty()) return error(ErrorCode::InvalidCommand, "Proposal has no model changes");
    Proposal proposal{ProposalId{QUuid::createUuid().toString(QUuid::WithoutBraces)}, base,
        storage::checksum(QByteArray::number(base) + '\n' + json::canonicalCommands(commands)), delta.diff, delta.diagnostics, delta.newIds};
    impl_->proposals.insert(proposal.id, {proposal, std::move(delta)});
    return proposal;
}
Outcome<Applied> Workspace::apply(const Approval &approval) {
    auto found = impl_->proposals.constFind(approval.id);
    if (found == impl_->proposals.cend() || approval.baseRevision != impl_->revision
        || approval.baseRevision != found->proposal.baseRevision || approval.digest != found->proposal.digest)
        return error(ErrorCode::StaleApproval, "Approval does not match an active proposal, digest and revision");
    auto revision = impl_->nextRevision(); if (auto e = std::get_if<WorkspaceError>(&revision)) return *e;
    const auto transaction = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto applied = impl_->model->applyDelta(found->delta); if (auto e = std::get_if<WorkspaceError>(&applied)) return *e;
    const auto ids = found->delta.newIds;
    impl_->undo.append({transaction, found->delta}); impl_->redo.clear(); impl_->changed();
    return Applied{impl_->revision, transaction, ids};
}
Status Workspace::reject(const ProposalId &id) {
    if (!impl_->proposals.remove(id)) return error(ErrorCode::StaleApproval, "Proposal is no longer active");
    return std::monostate{};
}
Outcome<Applied> Workspace::undo() {
    if (impl_->undo.isEmpty()) return error(ErrorCode::InvalidCommand, "Nothing to undo");
    auto revision = impl_->nextRevision(); if (auto e = std::get_if<WorkspaceError>(&revision)) return *e;
    const auto &entry = impl_->undo.back(); auto applied = impl_->model->applyDelta(entry.delta, false); if (auto e = std::get_if<WorkspaceError>(&applied)) return *e;
    Applied result{impl_->revision + 1, entry.id, entry.delta.newIds};
    impl_->redo.append(std::move(impl_->undo.back())); impl_->undo.removeLast(); impl_->changed(); return result;
}
Outcome<Applied> Workspace::redo() {
    if (impl_->redo.isEmpty()) return error(ErrorCode::InvalidCommand, "Nothing to redo");
    auto revision = impl_->nextRevision(); if (auto e = std::get_if<WorkspaceError>(&revision)) return *e;
    const auto &entry = impl_->redo.back(); auto applied = impl_->model->applyDelta(entry.delta); if (auto e = std::get_if<WorkspaceError>(&applied)) return *e;
    Applied result{impl_->revision + 1, entry.id, entry.delta.newIds};
    impl_->undo.append(std::move(impl_->redo.back())); impl_->redo.removeLast(); impl_->changed(); return result;
}
Status Workspace::grantDirectory(const QString &directory) {
    auto input = impl_->policy; input.allowedDirectories.append(directory);
    auto normalized = storage::canonicalPolicy(input); if (auto e = std::get_if<WorkspaceError>(&normalized)) return *e;
    impl_->policy = std::move(std::get<AccessPolicy>(normalized)); return std::monostate{};
}
Revision Workspace::revision() const { return impl_->revision; }
bool Workspace::dirty() const { return impl_->dirty; }
QString Workspace::path() const { return impl_->path; }
Outcome<SaveReceipt> Workspace::save(const SaveMode &mode) {
    auto serialized = impl_->model->serialize(); if (auto e = std::get_if<WorkspaceError>(&serialized)) return *e;
    const auto &bytes = std::get<QMap<UnitId, QByteArray>>(serialized);
    auto destination = storage::canonicalPath(mode.destination.isEmpty() ? impl_->path : mode.destination, impl_->policy, true);
    if (auto e = std::get_if<WorkspaceError>(&destination)) return *e;
    const auto target = std::get<QString>(destination); const bool relocating = target != impl_->path;
    QVector<storage::Write> writes;
    LoadedUnits relocated;
    std::unique_ptr<Model> relocatedModel;
    if (relocating) {
        // Source files and passive dependencies must still match the loaded set.
        for (const auto &document : impl_->units.documents) if (impl_->units.checksums.contains(document.id)) {
            auto current = storage::read(document.path, impl_->policy); if (auto e = std::get_if<WorkspaceError>(&current)) return *e;
            if (std::get<storage::Snapshot>(current).checksum != impl_->units.checksums.value(document.id)) return error(ErrorCode::DiskConflict, "Source unit changed before SaveAs", document.path);
        }
        auto moved = ControlledUnits::relocate(impl_->units, bytes, impl_->path, target, impl_->policy); if (auto e = std::get_if<WorkspaceError>(&moved)) return *e;
        writes = std::move(std::get<QVector<storage::Write>>(moved));
        // Validate the relocated native documents before touching any target.
        const QDir sourceBase(QFileInfo(impl_->path).absolutePath());
        const QDir destinationBase(QFileInfo(target).absolutePath());
        QMap<QString, bool> writablePaths;
        for (const auto &document : impl_->units.documents) {
            const auto proposed = document.path == impl_->path
                ? target : destinationBase.filePath(sourceBase.relativeFilePath(document.path));
            auto canonical = storage::canonicalPath(proposed, impl_->policy, true);
            if (auto e = std::get_if<WorkspaceError>(&canonical)) return *e;
            writablePaths.insert(std::get<QString>(canonical), document.writable);
        }
        for (const auto &write : writes) {
            // Native identity comes from the source set, not the destination suffix.
            if (writablePaths.contains(write.path)) {
                auto parsed = PetalDocument::parse(write.bytes, impl_->policy.sourceEncoding); if (auto e = std::get_if<WorkspaceError>(&parsed)) return *e;
                const UnitId id{write.path};
                const bool writable = writablePaths.value(write.path);
                relocated.documents.append({id, write.path, std::move(std::get<PetalDocument>(parsed)), writable});
                relocated.summaries.append({id, write.path, writable, true}); relocated.checksums.insert(id, storage::checksum(write.bytes));
            } else relocated.artifacts.append({write.path, write.bytes, storage::checksum(write.bytes)});
        }
        auto model = Model::fromDocuments(relocated.documents); if (auto e = std::get_if<WorkspaceError>(&model)) return *e;
        relocatedModel = std::move(std::get<std::unique_ptr<Model>>(model));
    } else {
        for (const auto &document : impl_->units.documents) {
            if (!bytes.contains(document.id)) return error(ErrorCode::UnsafeRewrite, "Serializer omitted a loaded unit", document.path);
            const auto emitted = bytes.value(document.id);
            if (!document.writable && emitted != document.document.bytes()) return error(ErrorCode::AccessDenied, "Reference model cannot be saved with mutations", document.path);
            if (!document.writable) continue;
            // Missing dependencies are preserved for no-op; mutation requires a complete set.
            if (emitted != document.document.bytes()) for (const auto &reference : impl_->units.references) if (!reference.resolved)
                return error(ErrorCode::MissingUnit, "Mutation cannot be saved with unresolved dependencies", reference.spelling);
            writes.append({document.path, emitted, impl_->units.checksums.contains(document.id) ? std::optional<QByteArray>(impl_->units.checksums.value(document.id)) : std::nullopt});
        }
    }
    auto saved = storage::save(writes, target, impl_->policy); if (auto e = std::get_if<WorkspaceError>(&saved)) return *e;
    if (relocating) {
        // Reload reference metadata from the verified saved set; no further model mutation.
        auto loaded = ControlledUnits::load(target, impl_->policy);
        if (auto e = std::get_if<WorkspaceError>(&loaded)) return *e;
        impl_->units = std::move(std::get<LoadedUnits>(loaded)); impl_->model = std::move(relocatedModel); impl_->path = target;
        impl_->undo.clear(); impl_->redo.clear(); impl_->proposals.clear();
    } else {
        const auto &checksums = std::get<QMap<QString, QByteArray>>(saved);
        for (const auto &document : impl_->units.documents) if (checksums.contains(document.path)) impl_->units.checksums.insert(document.id, checksums.value(document.path));
    }
    impl_->dirty = false;
    return SaveReceipt{impl_->revision, impl_->units.checksums, impl_->path};
}
} // namespace rose
