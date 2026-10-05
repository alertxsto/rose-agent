#include "desktop/MainWindow.h"
#include "desktop/DiagramScene.h"
#include "agent/Credentials.h"
#include <QApplication>
#include <QBuffer>
#include <QDialog>
#include <QImage>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QPixmap>
#include <QSslError>
#include <QNetworkReply>
#include <QPushButton>
#include <QSslSocket>
#include <QTabWidget>
#include <QTimer>
#include <QUuid>
#include <cstdio>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("RoseAgentReleaseSmoke");
    app.setApplicationName("IsolatedReleaseProbe");
    if (argc != 4) {
        std::fprintf(stderr, "Usage: rose-release-probe native.mdl screenshot.png https-url\n");
        return 2;
    }
    for (const QByteArray format : {QByteArray("PNG"), QByteArray("JPEG"), QByteArray("WEBP")}) {
        QImage image(7, 5, QImage::Format_RGB32);
        image.fill(QColor(12, 34, 56));
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, format.constData())) {
            std::fprintf(stderr, "Missing packaged encoder: %s\n", format.constData());
            return 1;
        }
        const QImage reopened = QImage::fromData(bytes, format.constData());
        if (reopened.size() != image.size() || reopened.pixelColor(3, 2).alpha() != 255) {
            std::fprintf(stderr, "Packaged image decode failed: %s\n", format.constData());
            return 1;
        }
    }
    if (!QSslSocket::supportsSsl()) {
        std::fprintf(stderr, "Packaged Qt TLS backend unavailable\n");
        return 1;
    }
#ifdef Q_OS_WIN
    const QString reference = "release-smoke-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QByteArray expected = QUuid::createUuid().toRfc4122().toHex();
    if (rose::agent::Credentials::store(reference, expected)) {
        rose::agent::Credentials::remove(reference);
        expected.fill('\0');
        std::fprintf(stderr, "Actual Windows Credential Manager store failed\n");
        return 1;
    }
    auto actual = rose::agent::Credentials::read(reference);
    const auto removal = rose::agent::Credentials::remove(reference);
    auto *secret = std::get_if<QByteArray>(&actual);
    const bool matched = secret && *secret == expected;
    if (secret) secret->fill('\0');
    expected.fill('\0');
    if (!matched || removal) {
        std::fprintf(stderr, "Actual Windows Credential Manager read/remove failed\n");
        return 1;
    }
    std::puts("PACKAGED_WINDOWS_CREDENTIAL_PASS real OS store/read/remove of isolated generated reference");
#endif
    rose::desktop::MainWindow window;
    window.show();
    QNetworkAccessManager network;
    auto *reply = network.get(QNetworkRequest(QUrl(QString::fromLocal8Bit(argv[3]))));
    bool tls = false, canvas = false, failed = false;
    auto fail = [&](const QString &reason) {
        if (failed) return;
        failed = true;
        std::fprintf(stderr, "RELEASE_PROBE_FAIL %s\n", reason.toUtf8().constData());
        app.exit(1);
    };
    QObject::connect(reply, &QNetworkReply::sslErrors, &app, [&](const QList<QSslError> &) {
        fail("TLS certificate validation failed; no ignoreSslErrors fallback");
    });
    QObject::connect(reply, &QNetworkReply::finished, &app, [&] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 400) {
            fail("Real HTTPS failed: " + reply->errorString());
            return;
        }
        tls = true;
        std::printf("PACKAGED_TLS_PASS Qt=%s TLS=%s HTTP=%d\n", qVersion(),
                    QSslSocket::sslLibraryVersionString().toUtf8().constData(), status);
        reply->deleteLater();
    });
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout, &app, [&] {
        if (failed) return;
        if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
            if (dialog->objectName() == "NativeFilePolicyDialog") {
                dialog->findChild<QPushButton *>("AcceptNativeFilePolicy")->click();
                return;
            }
            fail("Unexpected release-probe modal: " + dialog->windowTitle());
            return;
        }
        auto *tabs = window.findChild<QTabWidget *>("DiagramTabs");
        if (!canvas && tabs && tabs->count()) {
            auto *view = qobject_cast<rose::desktop::DiagramView *>(tabs->currentWidget());
            if (view && view->diagramScene()->projection().presentations.size() == 3) {
                if (!window.grab().save(QString::fromLocal8Bit(argv[2]))) {
                    fail("Actual native GUI screenshot could not be saved");
                    return;
                }
                canvas = true;
            }
        }
        if (tls && canvas) {
            std::puts("PACKAGED_UI_PASS actual native canvas, PNG/JPEG/WebP codecs and validated HTTPS");
            std::fflush(stdout);
            driver.stop();
            app.exit(0);
        }
    });
    driver.start(100);
    QTimer::singleShot(0, &window, [&] { window.openPath(QString::fromLocal8Bit(argv[1])); });
    QTimer::singleShot(60000, &app, [&] { fail("Actual packaged UI/TLS probe timed out"); });
    return app.exec();
}
