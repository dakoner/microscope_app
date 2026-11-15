#ifndef MINDVISIONCAMERASERVER_H
#define MINDVISIONCAMERASERVER_H

#include <QObject>
#include <QString>
#include <QtGlobal>

#include "mindvision_qobject_global.h"

class MindVisionCamera;
class VideoThread;
class MindVisionCameraServerPrivate;

class MINDVISION_QOBJECT_EXPORT MindVisionCameraServer : public QObject
{
    Q_OBJECT
public:
    explicit MindVisionCameraServer(QObject *parent = nullptr);
    ~MindVisionCameraServer() override;

    // Network listener control
    Q_INVOKABLE bool startServer(const QString &bindAddress = QStringLiteral("0.0.0.0"),
                                 quint16 port = 8766);
    Q_INVOKABLE void stopServer();
    Q_INVOKABLE bool isListening() const;
    Q_INVOKABLE QString bindAddress() const;
    Q_INVOKABLE quint16 serverPort() const;
    Q_INVOKABLE int connectedClientCount() const;

    // Optional synthetic camera mode for testing without hardware
    Q_INVOKABLE void setSyntheticMode(bool enabled);
    Q_INVOKABLE bool isSyntheticMode() const;

    // Default JPEG quality (1..100 = JPEG, 0 = uncompressed Raw RGB888)
    Q_INVOKABLE void setJpegQuality(int quality);
    Q_INVOKABLE int jpegQuality() const;

    // Default directory for server-side video recordings
    Q_INVOKABLE void setRecordingDirectory(const QString &dirPath);
    Q_INVOKABLE QString recordingDirectory() const;

    // Keep camera running even when all clients disconnect
    Q_INVOKABLE void setKeepCameraOpen(bool keepOpen);
    Q_INVOKABLE bool keepCameraOpen() const;

    // Direct hardware camera control on the server
    Q_INVOKABLE bool openCamera();
    Q_INVOKABLE void closeCamera();
    Q_INVOKABLE bool startCapture();
    Q_INVOKABLE void stopCapture();

    MindVisionCamera *camera() const;

signals:
    void clientConnected(QString peerAddress);
    void clientDisconnected(QString peerAddress);
    void logMessage(QString message);
    void errorOccurred(QString message);

private:
    MindVisionCameraServerPrivate *d;
};

#endif // MINDVISIONCAMERASERVER_H
