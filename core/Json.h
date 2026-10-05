#pragma once

#include "Workspace.h"
#include <QJsonArray>
#include <QJsonObject>

namespace rose::json {
Outcome<Query> query(const QJsonObject &);
Outcome<QVector<Command>> commands(const QJsonArray &);
Outcome<Approval> approval(const QJsonObject &);
Outcome<Revision> revision(const QJsonValue &);
QJsonObject query(const Query &);
QJsonArray commands(const QVector<Command> &);
QJsonObject projection(const Projection &);
QJsonObject proposal(const Proposal &);
QJsonObject approval(const Approval &);
QJsonObject applied(const Applied &);
QJsonObject saved(const SaveReceipt &);
QJsonObject error(const WorkspaceError &);
QJsonObject reply(const WorkspaceReply &);
QByteArray canonicalCommands(const QVector<Command> &);
QJsonArray tools();
} // namespace rose::json
