#include "desktop/MainWindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QTimer>

int main(int argc, char **argv) {
    // Qt 6 enables high-DPI scaling and device-independent widget coordinates by default.
    QCoreApplication::setOrganizationName(QStringLiteral("RoseAgent"));
    QCoreApplication::setApplicationName(QStringLiteral("RoseAgent"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication app(argc, argv);
    app.setApplicationDisplayName(QStringLiteral("Rose Agent"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Native UML model editor with reviewed software-engineering AI assistance. Manual editing and saving work offline."));
    parser.addHelpOption(); parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("model"), QStringLiteral("Optional existing Rational Rose .mdl to open."), QStringLiteral("[model.mdl]"));
    parser.process(app);
    const auto paths = parser.positionalArguments();
    if (paths.size() > 1) parser.showHelp(1);
    rose::desktop::MainWindow window;
    window.show();
    if (!paths.isEmpty()) {
        const auto path = QFileInfo(paths.first()).absoluteFilePath();
        QTimer::singleShot(0, &window, [&window, path] { window.openPath(path); });
    }
    return app.exec();
}
