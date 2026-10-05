#include "agent/AgentTools.h"
#include "core/Json.h"
#include <QJsonDocument>
#include <QtTest>
using namespace rose;
using namespace rose::agent;
class PermissionTests final : public QObject {
    Q_OBJECT
private slots:
    void forbiddenCallsHaveNoDispatchCapability_data() {
        QTest::addColumn<QString>("name");
        for (const auto &name : {"apply", "save", "delete", "shell", "fetch_url", "approve_proposal"}) QTest::newRow(name) << QString(name);
    }
    void forbiddenCallsHaveNoDispatchCapability() {
        QFETCH(QString, name);
        const auto parsed = AgentTools::parse({"call", name, "{}"}, 7);
        QVERIFY(std::holds_alternative<AgentError>(parsed));
        QCOMPARE(std::get<AgentError>(parsed).code, AgentErrorCode::PermissionDenied);
    }
    void malformedToolArgumentsAreRejected() {
        const auto result = AgentTools::parse({"call", "inspect", "not json"}, 7);
        QCOMPARE(std::get<AgentError>(result).code, AgentErrorCode::InvalidArguments);
    }
    void proposalCannotChangeObservedBaseRevision() {
        const auto result = AgentTools::parse({"call", "submit_proposal", "{\"baseRevision\":\"8\",\"commands\":[]}"}, 7);
        QCOMPARE(std::get<AgentError>(result).code, AgentErrorCode::StaleRevision);
    }
    void closedQueryRejectsEmbeddedAuthority() {
        auto query = json::query(Query{});
        query.insert("apply", true);
        const auto result = AgentTools::parse({"call", "inspect", QJsonDocument(query).toJson(QJsonDocument::Compact)}, 7);
        QVERIFY(std::holds_alternative<AgentError>(result));
    }
    void invalidSecondCommandCannotDispatchPartialBatch() {
        QJsonArray commands = json::commands({Command{RenameElement{ElementId{"id"}, "Changed"}}});
        commands.append(QJsonObject{{"type", "executeShell"}, {"command", "rm"}});
        const auto bytes = QJsonDocument(QJsonObject{{"baseRevision", "7"}, {"commands", commands}}).toJson(QJsonDocument::Compact);
        const auto result = AgentTools::parse({"call", "submit_proposal", bytes}, 7);
        QCOMPARE(std::get<AgentError>(result).code, AgentErrorCode::InvalidArguments);
    }
};
QTEST_GUILESS_MAIN(PermissionTests)
#include "tst_permissions.moc"
