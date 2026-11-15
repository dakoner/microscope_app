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
        {QStringLiteral("remote-camera"), QStringLiteral("remote"), QStringLiteral("remote-stage")},
        QStringLiteral("Connect camera and stage to a remote camera_server (HOST or HOST:PORT)."),
        QStringLiteral("endpoint"));
    const QCommandLineOption cameraModeOption(
        QStringLiteral("camera-mode"),
        QStringLiteral("Hardware mode: local or remote."),
        QStringLiteral("mode"));
    const QCommandLineOption cameraHostOption(
        QStringLiteral("camera-host"),
        QStringLiteral("Remote camera_server hostname or IP."),
        QStringLiteral("host"));
    const QCommandLineOption cameraPortOption(
        QStringLiteral("camera-port"),
        QStringLiteral("Remote camera_server TCP port (default: 8766)."),
        QStringLiteral("port"));
    const QCommandLineOption jpegQualityOption(
        QStringLiteral("camera-jpeg-quality"),
        QStringLiteral("Remote camera JPEG quality 1..100, or 0 for raw RGB888."),
        QStringLiteral("quality"));
    const QCommandLineOption stagePortOption(
        QStringLiteral("stage-port"),
        QStringLiteral("Serial port name to connect the CNC stage controller to."),
        QStringLiteral("port_name"));
    const QCommandLineOption autoConnectStageOption(
        QStringLiteral("auto-connect-stage"),
        QStringLiteral("Automatically connect to the CNC stage serial port on startup."));

    parser.addOption(remoteCameraOption);
    parser.addOption(cameraModeOption);
    parser.addOption(cameraHostOption);
    parser.addOption(cameraPortOption);
    parser.addOption(jpegQualityOption);
    parser.addOption(stagePortOption);
    parser.addOption(autoConnectStageOption);
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
    if (parser.isSet(stagePortOption)) {
        qputenv("MICROSCOPE_STAGE_PORT", parser.value(stagePortOption).toUtf8());
    }
    if (parser.isSet(autoConnectStageOption)) {
        qputenv("MICROSCOPE_STAGE_AUTO_CONNECT", "1");
    }

    MainWindow window;
    window.showMaximized();

    return app.exec();
}
