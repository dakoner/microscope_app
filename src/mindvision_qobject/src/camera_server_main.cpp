#include "MindVisionCameraServer.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <csignal>
#include <iostream>

int main(int argc, char *argv[])
{
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);

    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("MindVision Headless Camera Server"));
    app.setOrganizationName(QStringLiteral("MicroTools"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Headless TCP server for streaming MindVision camera frames and control commands."));
    parser.addHelpOption();

    const QCommandLineOption hostOption(
        {QStringLiteral("b"), QStringLiteral("bind"), QStringLiteral("host")},
        QStringLiteral("Bind address (default: 0.0.0.0)."),
        QStringLiteral("address"),
        QStringLiteral("0.0.0.0"));
    const QCommandLineOption portOption(
        {QStringLiteral("p"), QStringLiteral("port")},
        QStringLiteral("TCP port to listen on (default: 8766)."),
        QStringLiteral("port"),
        QStringLiteral("8766"));
    const QCommandLineOption qualityOption(
        {QStringLiteral("q"), QStringLiteral("jpeg-quality")},
        QStringLiteral("Default JPEG quality 1..100, or 0 for raw RGB888 (default: 85)."),
        QStringLiteral("quality"),
        QStringLiteral("85"));
    const QCommandLineOption recordDirOption(
        {QStringLiteral("r"), QStringLiteral("record-dir")},
        QStringLiteral("Directory for server-side recordings (default: videos)."),
        QStringLiteral("dir"),
        QStringLiteral("videos"));
    const QCommandLineOption autoStartOption(
        QStringLiteral("auto-start"),
        QStringLiteral("Open and start the camera immediately on server startup."));
    const QCommandLineOption keepOpenOption(
        QStringLiteral("keep-open"),
        QStringLiteral("Keep the camera open/running even when all clients disconnect."));
    const QCommandLineOption syntheticOption(
        QStringLiteral("synthetic"),
        QStringLiteral("Use synthetic test-pattern generator instead of physical MindVision hardware."));

    parser.addOption(hostOption);
    parser.addOption(portOption);
    parser.addOption(qualityOption);
    parser.addOption(recordDirOption);
    parser.addOption(autoStartOption);
    parser.addOption(keepOpenOption);
    parser.addOption(syntheticOption);
    parser.process(app);

    const QString bindAddr = parser.value(hostOption);
    const quint16 port = static_cast<quint16>(parser.value(portOption).toUShort());
    const int jpegQuality = parser.value(qualityOption).toInt();
    const QString recordDir = parser.value(recordDirOption);
    const bool autoStart = parser.isSet(autoStartOption);
    const bool keepOpen = parser.isSet(keepOpenOption) || autoStart;
    const bool synthetic = parser.isSet(syntheticOption);

    MindVisionCameraServer server;
    server.setSyntheticMode(synthetic);
    server.setJpegQuality(jpegQuality);
    server.setRecordingDirectory(recordDir);
    server.setKeepCameraOpen(keepOpen);

    QObject::connect(&server, &MindVisionCameraServer::logMessage, [](const QString &msg) {
        const QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
        std::cout << "[" << ts.toStdString() << "] " << msg.toStdString() << std::endl;
    });
    QObject::connect(&server, &MindVisionCameraServer::errorOccurred, [](const QString &msg) {
        const QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
        std::cerr << "[" << ts.toStdString() << "] ERROR: " << msg.toStdString() << std::endl;
    });

    if (autoStart) {
        if (!server.startCapture()) {
            std::cerr << "Warning: failed to auto-start camera on startup (will retry on client connect)."
                      << std::endl;
        }
    }

    if (!server.startServer(bindAddr, port)) {
        return 1;
    }

    return app.exec();
}
