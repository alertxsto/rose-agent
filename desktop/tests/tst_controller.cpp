#include "desktop/WorkspaceController.h"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace rose;
using namespace rose::desktop;
static ElementId logicalOwner(const WorkspaceReply &reply) {
    const auto &projection = std::get<Projection>(reply);
    for (const auto &element : projection.elements)
        if (element.kind == "Class_Category" && element.name == "Logical View") return element.id;
    return {};
}
class ControllerTest : public QObject {
    Q_OBJECT
private slots:
    void replacedRequestCompletesWithStaleError() {
        QTemporaryDir dir;
        AccessPolicy access; access.allowedDirectories = {dir.path()};
        WorkspaceController controller;
        QSignalSpy replies(&controller, &WorkspaceController::completed);
        const auto older = controller.create(dir.filePath("older.mdl"), {}, access);
        const auto newer = controller.create(dir.filePath("newer.mdl"), {}, access);
        QTRY_COMPARE(replies.size(), 2);
        WorkspaceReply oldResult, newResult;
        for (const auto &row : replies) {
            if (row[0].toULongLong() == older) oldResult = qvariant_cast<WorkspaceReply>(row[1]);
            if (row[0].toULongLong() == newer) newResult = qvariant_cast<WorkspaceReply>(row[1]);
        }
        QVERIFY(std::holds_alternative<WorkspaceError>(oldResult));
        QCOMPARE(std::get<WorkspaceError>(oldResult).code, ErrorCode::StaleRevision);
        QVERIFY(std::holds_alternative<Projection>(newResult));
        QCOMPARE(std::get<Projection>(newResult).path, dir.filePath("newer.mdl"));
        const auto queried = controller.inspect({});
        QTRY_COMPARE(replies.last()[0].toULongLong(), queried);
        QCOMPARE(std::get<Projection>(qvariant_cast<WorkspaceReply>(replies.last()[1])).path,
                 dir.filePath("newer.mdl"));
    }
    void queuedWorkspaceAndRejectedAreDistinct() {
        QTemporaryDir dir;
        AccessPolicy access; access.allowedDirectories = {dir.path()};
        WorkspaceController controller;
        QSignalSpy replies(&controller, &WorkspaceController::completed);
        auto created = controller.create(dir.filePath("model.mdl"), {}, access);
        QTRY_COMPARE(replies.size(), 1);
        QCOMPARE(replies[0][0].toULongLong(), created);
        QVERIFY(std::holds_alternative<Projection>(qvariant_cast<WorkspaceReply>(replies[0][1])));
        auto proposed = controller.propose(controller.revision(),
            {{CreateElement{"new", "Class", "Order", logicalOwner(qvariant_cast<WorkspaceReply>(replies[0][1])), {}}}});
        QTRY_COMPARE(replies.size(), 2);
        QCOMPARE(replies[1][0].toULongLong(), proposed);
        auto reply = qvariant_cast<WorkspaceReply>(replies[1][1]);
        QVERIFY(std::holds_alternative<Proposal>(reply));
        controller.reject(std::get<Proposal>(reply).id);
        QTRY_COMPARE(replies.size(), 3);
        QVERIFY(std::holds_alternative<Rejected>(qvariant_cast<WorkspaceReply>(replies[2][1])));
        QVERIFY(controller.hasWorkspace());
        controller.close();
        QTRY_COMPARE(replies.size(), 4);
        QVERIFY(std::holds_alternative<Closed>(qvariant_cast<WorkspaceReply>(replies[3][1])));
        QVERIFY(!controller.hasWorkspace());
    }
    void newerSessionSupersedesQueuedCreate() {
        QTemporaryDir dir;
        AccessPolicy access; access.allowedDirectories = {dir.path()};
        WorkspaceController controller;
        QSignalSpy replies(&controller, &WorkspaceController::completed);
        controller.create(dir.filePath("older.mdl"), {}, access);
        const auto newer = controller.create(dir.filePath("newer.mdl"), {}, access);
        QTRY_VERIFY(!replies.isEmpty() && replies.last()[0].toULongLong() == newer);
        const auto value = qvariant_cast<WorkspaceReply>(replies.last()[1]);
        QVERIFY(std::holds_alternative<Projection>(value));
        QCOMPARE(std::get<Projection>(value).path, dir.filePath("newer.mdl"));
        const auto query = controller.inspect({});
        QTRY_COMPARE(replies.last()[0].toULongLong(), query);
        QCOMPARE(std::get<Projection>(qvariant_cast<WorkspaceReply>(replies.last()[1])).path,
                 dir.filePath("newer.mdl"));
    }
    void rejectedProposalCannotBeApplied() {
        QTemporaryDir dir;
        AccessPolicy access; access.allowedDirectories = {dir.path()};
        WorkspaceController controller;
        QSignalSpy replies(&controller, &WorkspaceController::completed);
        controller.create(dir.filePath("model.mdl"), {}, access);
        QTRY_COMPARE(replies.size(), 1);
        controller.propose(controller.revision(),
            {{CreateElement{"new", "Class", "Order", logicalOwner(qvariant_cast<WorkspaceReply>(replies[0][1])), {}}}});
        QTRY_COMPARE(replies.size(), 2);
        const auto result = qvariant_cast<WorkspaceReply>(replies[1][1]);
        QVERIFY(std::holds_alternative<Proposal>(result));
        const auto proposal = std::get<Proposal>(result);
        controller.reject(proposal.id);
        QTRY_COMPARE(replies.size(), 3);
        const auto revision = controller.revision();
        controller.apply({proposal.id, proposal.baseRevision, proposal.digest});
        QTRY_COMPARE(replies.size(), 4);
        QVERIFY(std::holds_alternative<WorkspaceError>(qvariant_cast<WorkspaceReply>(replies[3][1])));
        QCOMPARE(controller.revision(), revision);
        QVERIFY(controller.hasWorkspace());
    }
};
QTEST_GUILESS_MAIN(ControllerTest)
#include "tst_controller.moc"
