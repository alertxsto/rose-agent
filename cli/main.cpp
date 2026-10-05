#include "core/Json.h"
#include "core/Storage.h"
#include "CliSession.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <cstdio>
#include <limits>

using namespace rose;
namespace {
WorkspaceError invalid(QString message) { return {ErrorCode::InvalidCommand, std::move(message), {}}; }
WorkspaceError ioError(QString message, QString file = {}) { return {ErrorCode::StorageFailure, std::move(message), std::move(file)}; }
bool emitJson(QFile &output, const QJsonObject &value) {
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n';
    return output.write(bytes) == bytes.size() && output.flush();
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); QCoreApplication::setApplicationName("rose-cli"); QCoreApplication::setApplicationVersion("0.1.0");
    QCommandLineParser parser; parser.setApplicationDescription("Inspect and transactionally edit native Rational Rose models"); parser.addHelpOption(); parser.addVersionOption();
    parser.addPositionalArgument("command", "inspect, roundtrip, or session");
    parser.addOption({{"f", "file"}, "Native model to open", "file"});
    parser.addOption({{"o", "output"}, "Inspection JSON file or roundtrip native destination", "file"});
    parser.addOption({"allow-root", "Allowed model/dependency/output directory (repeatable)", "directory"});
    parser.addOption({"path-variable", "Controlled-unit path mapping NAME=directory (repeatable)", "mapping"});
    parser.addOption({"source-encoding", "Explicit native source codec; default ASCII", "codec", "ASCII"});
    parser.addOption({"read-only", "Refuse modifications and native saves"});
    parser.process(app);
    QFile output, errors; if (!output.open(stdout, QIODevice::WriteOnly) || !errors.open(stderr, QIODevice::WriteOnly)) return 1;
    auto fail = [&](WorkspaceError error) { emitJson(errors, json::error(error)); return 1; };
    const auto positional = parser.positionalArguments();
    if (positional.size() != 1 || (positional.first() != "inspect" && positional.first() != "roundtrip" && positional.first() != "session")) return fail(invalid("Specify exactly one command: inspect, roundtrip, session"));
    const auto command = positional.first(), file = parser.value("file");
    if (command != "session" && file.isEmpty()) return fail(invalid("--file is required"));
    if (command == "session" && parser.isSet("output")) return fail(invalid("session responses use stdout; --output is not supported"));
    AccessPolicy policy; policy.allowedDirectories = parser.values("allow-root"); policy.sourceEncoding = parser.value("source-encoding").toLatin1(); policy.writable = !parser.isSet("read-only");
    if (policy.sourceEncoding.isEmpty()) return fail(invalid("--source-encoding must not be empty"));
    if (policy.allowedDirectories.isEmpty()) policy.allowedDirectories.append(file.isEmpty() ? QDir::currentPath() : QFileInfo(file).absolutePath());
    for (const auto &mapping : parser.values("path-variable")) {
        auto separator = mapping.indexOf('='); if (separator <= 0 || separator == mapping.size() - 1) return fail(invalid("--path-variable requires NAME=directory"));
        auto name = mapping.left(separator); if (policy.pathVariables.contains(name)) return fail(invalid("Duplicate path variable: " + name)); policy.pathVariables.insert(name, mapping.mid(separator + 1));
    }
    auto canonicalPolicy = storage::canonicalPolicy(policy);
    if (auto error = std::get_if<WorkspaceError>(&canonicalPolicy)) return fail(*error);
    policy = std::get<AccessPolicy>(std::move(canonicalPolicy));
    if (command == "session") {
        rose::cli::CliSession state(policy, output, errors);
        QObject::connect(&state, &rose::cli::CliSession::finished, &app, &QCoreApplication::exit);
        state.start(file);
        return app.exec();
    }
    auto opened = Workspace::open(file, policy);
    if (auto error = std::get_if<WorkspaceError>(&opened)) return fail(*error);
    auto workspace = std::get<std::unique_ptr<Workspace>>(std::move(opened));
    QJsonObject json;
    if (command == "roundtrip") {
        QString destination = parser.value("output");
        if (destination.isEmpty()) {
            QFileInfo source(file); destination = source.dir().filePath(source.completeBaseName() + ".roundtrip." + (source.suffix().isEmpty() ? QStringLiteral("mdl") : source.suffix()));
            if (QFileInfo::exists(destination)) return fail(ioError("Default roundtrip destination already exists; choose an explicit --output", destination));
        }
        auto saved = workspace->save({destination});
        if (auto error = std::get_if<WorkspaceError>(&saved)) return fail(*error);
        json = rose::json::saved(std::get<SaveReceipt>(saved));
    } else {
        Query query; query.limit = std::numeric_limits<qsizetype>::max();
        auto inspected = workspace->inspect(query);
        if (auto error = std::get_if<WorkspaceError>(&inspected)) return fail(*error);
        json = rose::json::projection(std::get<Projection>(inspected));
    }
    if (command == "inspect" && parser.isSet("output")) {
        auto resolved = storage::canonicalPath(parser.value("output"), policy, true);
        if (auto error = std::get_if<WorkspaceError>(&resolved)) return fail(*error);
        const auto destination = std::get<QString>(resolved);
        auto source = storage::canonicalPath(file, policy);
        if (auto error = std::get_if<WorkspaceError>(&source)) return fail(*error);
        if (destination == std::get<QString>(source)) return fail(invalid("Inspection output must not overwrite the native source"));
        QSaveFile target(destination); target.setDirectWriteFallback(false);
        if (!target.open(QIODevice::WriteOnly)) return fail(ioError(target.errorString(), destination));
        const auto bytes = QJsonDocument(json).toJson(QJsonDocument::Indented); if (target.write(bytes) != bytes.size() || !target.commit()) return fail(ioError(target.errorString(), destination));
        return 0;
    }
    if (!emitJson(output, json)) return fail(ioError("Cannot write JSON output"));
    return 0;
}
