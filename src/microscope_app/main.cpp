#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <csignal>
#include "MainWindow.h"

int main(int argc, char *argv[])
{
    // Enable Ctrl+C termination
    std::signal(SIGINT, SIG_DFL);

    QApplication app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Microscope Application"));
    parser.addHelpOption();

    const QCommandLineOption remoteCameraOption(
        QStringLiteral("remote-camera"),
        QStringLiteral("Connect to a remote headless camera server (HOST or HOST:PORT)."),
        QStringLiteral("endpoint"));
    const QCommandLineOption cameraModeOption(
        QStringLiteral("camera-mode"),
        QStringLiteral("Camera mode: local or remote."),
        QStringLiteral("mode"));
    const QCommandLineOption cameraHostOption(
        QStringLiteral("camera-host"),
        QStringLiteral("Remote camera server hostname or IP."),
        QStringLiteral("host"));
    const QCommandLineOption cameraPortOption(
        QStringLiteral("camera-port"),
        QStringLiteral("Remote camera server TCP port (default: 8766)."),
        QStringLiteral("port"));
    const QCommandLineOption jpegQualityOption(
        QStringLiteral("camera-jpeg-quality"),
        QStringLiteral("Remote camera JPEG quality 1..100, or 0 for raw RGB888."),
        QStringLiteral("quality"));

    parser.addOption(remoteCameraOption);
    parser.addOption(cameraModeOption);
    parser.addOption(cameraHostOption);
    parser.addOption(cameraPortOption);
    parser.addOption(jpegQualityOption);
    parser.process(app);

    if (parser.isSet(remoteCameraOption)) {
        const QString ep = parser.value(remoteCameraOption).trimmed();
        const int colon = ep.lastIndexOf(':');
        if (colon > 0) {
            qputenv("MICROSCOPE_CAMERA_HOST", ep.left(colon).toUtf8());
            qputenv("MICROSCOPE_CAMERA_PORT", ep.mid(colon + 1).toUtf8());
        } else {
            qputenv("MICROSCOPE_CAMERA_HOST", ep.toUtf8());
        }
        qputenv("MICROSCOPE_CAMERA_MODE", "remote");
    }
    if (parser.isSet(cameraModeOption)) {
        qputenv("MICROSCOPE_CAMERA_MODE", parser.value(cameraModeOption).toUtf8());
    }
    if (parser.isSet(cameraHostOption)) {
        qputenv("MICROSCOPE_CAMERA_HOST", parser.value(cameraHostOption).toUtf8());
    }
    if (parser.isSet(cameraPortOption)) {
        qputenv("MICROSCOPE_CAMERA_PORT", parser.value(cameraPortOption).toUtf8());
    }
    if (parser.isSet(jpegQualityOption)) {
        qputenv("MICROSCOPE_CAMERA_JPEG_QUALITY", parser.value(jpegQualityOption).toUtf8());
    }

    MainWindow window;
    window.showMaximized();

    return app.exec();
}
