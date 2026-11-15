#include "MindVisionCameraServer.h"
#include "CameraNetworkProtocol.h"
#include "MindVisionCamera.h"
#include "VideoThread.h"

#include <QAbstractSocket>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cstring>

namespace {
// Maximum queued bytes per client socket before dropping preview frames for that client
constexpr qint64 kMaxPendingSocketBytes = 2 * 1024 * 1024;
} // namespace

// =============================================================================
// ServerFrameEncoder: encodes frames on a worker thread so the control loop
// never blocks on JPEG compression.
// =============================================================================

class ServerFrameEncoder : public QObject
{
    Q_OBJECT
public:
    explicit ServerFrameEncoder(QObject *parent = nullptr)
        : QObject(parent)
    {}

    void submitFrame(const QImage &image,
                     const CameraNet::FramePacketHeader &hdr,
                     int jpegQuality)
    {
        bool shouldTrigger = false;
        {
            QMutexLocker locker(&m_mutex);
            m_latestImage = image;
            m_latestHdr = hdr;
            m_latestJpegQuality = jpegQuality;
            if (!m_encodingPending) {
                m_encodingPending = true;
                shouldTrigger = true;
            }
        }
        if (shouldTrigger) {
            QMetaObject::invokeMethod(this, &ServerFrameEncoder::encodeNext, Qt::QueuedConnection);
        }
    }

public slots:
    void encodeNext()
    {
        QImage img;
        CameraNet::FramePacketHeader hdr;
        int quality = 85;
        {
            QMutexLocker locker(&m_mutex);
            img = m_latestImage;
            m_latestImage = QImage();
            hdr = m_latestHdr;
            quality = m_latestJpegQuality;
            m_encodingPending = false;
        }

        if (img.isNull()) {
            return;
        }

        QImage rgb = (img.format() == QImage::Format_RGB888)
            ? img
            : img.convertToFormat(QImage::Format_RGB888);

        hdr.width = static_cast<quint32>(rgb.width());
        hdr.height = static_cast<quint32>(rgb.height());

        QByteArray encodedBytes;
        if (quality <= 0) {
            hdr.encoding = CameraNet::kFrameEncodingRawRgb;
            const int rowBytes = rgb.width() * 3;
            encodedBytes.resize(rowBytes * rgb.height());
            uchar *dst = reinterpret_cast<uchar *>(encodedBytes.data());
            for (int y = 0; y < rgb.height(); ++y) {
                std::memcpy(dst + y * rowBytes, rgb.constScanLine(y), static_cast<std::size_t>(rowBytes));
            }
        } else {
            hdr.encoding = CameraNet::kFrameEncodingJpeg;
            QBuffer buf(&encodedBytes);
            buf.open(QIODevice::WriteOnly);
            rgb.save(&buf, "JPG", std::clamp(quality, 1, 100));
        }

        if (!encodedBytes.isEmpty()) {
            const QByteArray packet = CameraNet::packFramePacket(hdr, encodedBytes);
            emit packetEncoded(packet);
        }
    }

signals:
    void packetEncoded(QByteArray packet);

private:
    QMutex m_mutex;
    QImage m_latestImage;
    CameraNet::FramePacketHeader m_latestHdr;
    int m_latestJpegQuality = 85;
    bool m_encodingPending = false;
};

// =============================================================================
// ClientSession
// =============================================================================

struct ClientSession {
    QTcpSocket *socket = nullptr;
    QByteArray readBuffer;
    bool streaming = false;
    int jpegQuality = 85;
    QString peerName;
};

// =============================================================================
// MindVisionCameraServerPrivate
// =============================================================================

class MindVisionCameraServerPrivate : public QObject
{
    Q_OBJECT
public:
    explicit MindVisionCameraServerPrivate(MindVisionCameraServer *owner)
        : QObject(owner),
          q(owner),
          m_camera(new MindVisionCamera(this)),
          m_videoThread(new VideoThread(this)),
          m_tcpServer(new QTcpServer(this)),
          m_telemetryTimer(new QTimer(this)),
          m_encoderThread(new QThread(this)),
          m_encoder(new ServerFrameEncoder())
    {
        // Ensure the underlying camera is always local on the server
        m_camera->setRemoteMode(false);

        m_encoder->moveToThread(m_encoderThread);
        connect(m_encoderThread, &QThread::finished, m_encoder, &QObject::deleteLater);
        connect(m_encoder, &ServerFrameEncoder::packetEncoded,
                this, &MindVisionCameraServerPrivate::broadcastFramePacket);
        m_encoderThread->start();

        connect(m_tcpServer, &QTcpServer::newConnection,
                this, &MindVisionCameraServerPrivate::onNewConnection);

        connect(m_camera, &MindVisionCamera::frameReady,
                this, &MindVisionCameraServerPrivate::onCameraFrameReady);
        connect(m_camera, &MindVisionCamera::fpsChanged,
                this, &MindVisionCameraServerPrivate::onCameraFpsChanged);
        connect(m_camera, &MindVisionCamera::queueStatsChanged,
                this, &MindVisionCameraServerPrivate::onCameraQueueStatsChanged);
        connect(m_camera, &MindVisionCamera::errorOccurred,
                this, &MindVisionCameraServerPrivate::onCameraError);

        connect(m_telemetryTimer, &QTimer::timeout,
                this, &MindVisionCameraServerPrivate::updateCachedTelemetry);
        m_telemetryTimer->start(200);
    }

    ~MindVisionCameraServerPrivate() override
    {
        stopServer();
        if (m_videoThread->isRunning()) {
            m_videoThread->stopRecording();
            m_videoThread->wait();
        }
        m_camera->stop();
        m_camera->close();

        if (m_encoderThread) {
            m_encoderThread->quit();
            m_encoderThread->wait();
        }
    }

    void stopServer()
    {
        if (m_tcpServer->isListening()) {
            m_tcpServer->close();
        }
        for (ClientSession *session : std::as_const(m_clients)) {
            if (session->socket) {
                session->socket->disconnect(this);
                session->socket->disconnectFromHost();
                session->socket->deleteLater();
            }
            delete session;
        }
        m_clients.clear();
    }

    QJsonObject buildCameraStateJson() const
    {
        QJsonObject obj;
        const bool open = m_camera->isOpen();
        const bool running = m_camera->isRunning();
        obj["is_open"] = open;
        obj["is_running"] = running;
        obj["remote_recording"] = m_videoThread->isRunning();
        obj["last_recording_file"] = m_lastRecordingFile;
        obj["jpeg_quality"] = m_jpegQuality;

        if (open) {
            double minExp = 0.0, maxExp = 0.0;
            m_camera->getExposureTimeRange(minExp, maxExp);
            int minGain = 0, maxGain = 0;
            m_camera->getAnalogGainRange(minGain, maxGain);

            obj["auto_exposure"] = m_camera->getAutoExposure();
            obj["exposure_time_ms"] = m_camera->getExposureTime();
            obj["analog_gain"] = m_camera->getAnalogGain();
            obj["ae_target"] = m_camera->getAeTarget();
            obj["exposure_min_ms"] = minExp;
            obj["exposure_max_ms"] = maxExp;
            obj["exposure_step_ms"] = m_camera->getExposureTimeStep();
            obj["gain_min"] = minGain;
            obj["gain_max"] = maxGain;
        }
        if (m_lastFrameWidth > 0 && m_lastFrameHeight > 0) {
            obj["width"] = m_lastFrameWidth;
            obj["height"] = m_lastFrameHeight;
        }
        return obj;
    }

    void broadcastJsonEvent(const QJsonObject &eventObj)
    {
        if (m_clients.isEmpty()) {
            return;
        }
        const QByteArray packet = CameraNet::packJsonPacket(eventObj);
        for (ClientSession *session : std::as_const(m_clients)) {
            if (session->socket && session->socket->state() == QAbstractSocket::ConnectedState) {
                session->socket->write(packet);
                session->socket->flush();
            }
        }
    }

public slots:
    void updateCachedTelemetry()
    {
        if (!m_camera->isOpen()) {
            return;
        }
        m_cachedAutoExposure = m_camera->getAutoExposure();
        m_cachedExposureMs = m_camera->getExposureTime();
        m_cachedAnalogGain = m_camera->getAnalogGain();
        m_cachedAeTarget = m_camera->getAeTarget();
    }

    void onNewConnection()
    {
        while (QTcpSocket *sock = m_tcpServer->nextPendingConnection()) {
            sock->setSocketOption(QAbstractSocket::LowDelayOption, 1);
            auto *session = new ClientSession;
            session->socket = sock;
            session->jpegQuality = m_jpegQuality;
            session->peerName = QString("%1:%2")
                                    .arg(sock->peerAddress().toString())
                                    .arg(sock->peerPort());
            m_clients.append(session);

            connect(sock, &QTcpSocket::readyRead, this, [this, session]() {
                onClientReadyRead(session);
            });
            connect(sock, &QTcpSocket::disconnected, this, [this, session]() {
                onClientDisconnected(session);
            });

            emit q->logMessage(QString("Client connected: %1").arg(session->peerName));
            emit q->clientConnected(session->peerName);
        }
    }

    void onClientDisconnected(ClientSession *session)
    {
        if (!m_clients.removeOne(session)) {
            return;
        }
        const QString peer = session->peerName;
        if (session->socket) {
            session->socket->deleteLater();
            session->socket = nullptr;
        }
        delete session;

        emit q->logMessage(QString("Client disconnected: %1").arg(peer));
        emit q->clientDisconnected(peer);

        // If no clients remain and keepCameraOpen is false, stop streaming
        bool anyStreaming = false;
        for (const ClientSession *c : std::as_const(m_clients)) {
            if (c->streaming) {
                anyStreaming = true;
                break;
            }
        }
        if (!anyStreaming && !m_keepCameraOpen && !m_videoThread->isRunning()) {
            if (m_camera->isRunning()) {
                m_camera->stop();
            }
        }
    }

    void onClientReadyRead(ClientSession *session)
    {
        if (!session || !session->socket) {
            return;
        }
        session->readBuffer.append(session->socket->readAll());

        while (session->readBuffer.size() >= CameraNet::kPacketHeaderSize) {
            const uchar *hdr = reinterpret_cast<const uchar *>(session->readBuffer.constData());
            const quint8 packetType = hdr[0];
            const quint32 payloadLen = qFromBigEndian<quint32>(hdr + 1);

            if (payloadLen > 16U * 1024U * 1024U) {
                session->socket->abort();
                return;
            }

            const int totalSize = CameraNet::kPacketHeaderSize + static_cast<int>(payloadLen);
            if (session->readBuffer.size() < totalSize) {
                break;
            }

            const QByteArray payload = session->readBuffer.mid(CameraNet::kPacketHeaderSize,
                                                               static_cast<int>(payloadLen));
            session->readBuffer.remove(0, totalSize);

            if (packetType == CameraNet::kPacketTypeJson) {
                handleClientCommand(session, payload);
            }
        }
    }

    void handleClientCommand(ClientSession *session, const QByteArray &payload)
    {
        const QJsonDocument doc = QJsonDocument::fromJson(payload);
        if (!doc.isObject()) {
            return;
        }
        const QJsonObject req = doc.object();
        const qint64 reqId = req.value("id").toInteger(0);
        const QString cmd = req.value("cmd").toString();
        const QJsonObject params = req.value("params").toObject();

        bool ok = false;
        QString errorMsg;
        QJsonObject extraResult;

        if (cmd == "open") {
            if (params.contains("jpeg_quality")) {
                session->jpegQuality = std::clamp(params.value("jpeg_quality").toInt(m_jpegQuality), 0, 100);
                m_jpegQuality = session->jpegQuality;
            }
            ok = m_camera->isOpen() ? true : m_camera->open();
            if (ok) {
                updateCachedTelemetry();
            } else {
                errorMsg = "Failed to open camera on remote host.";
            }
        } else if (cmd == "close") {
            session->streaming = false;
            bool anyOtherStreaming = false;
            for (const ClientSession *c : std::as_const(m_clients)) {
                if (c != session && c->streaming) {
                    anyOtherStreaming = true;
                    break;
                }
            }
            if (!anyOtherStreaming && !m_keepCameraOpen && !m_videoThread->isRunning()) {
                m_camera->stop();
                m_camera->close();
            }
            ok = true;
        } else if (cmd == "start") {
            if (params.contains("jpeg_quality")) {
                session->jpegQuality = std::clamp(params.value("jpeg_quality").toInt(m_jpegQuality), 0, 100);
                m_jpegQuality = session->jpegQuality;
            }
            if (!m_camera->isOpen()) {
                m_camera->open();
            }
            ok = m_camera->isRunning() ? true : m_camera->start();
            if (ok) {
                session->streaming = true;
                updateCachedTelemetry();
            } else {
                errorMsg = "Failed to start camera stream on remote host.";
            }
        } else if (cmd == "stop") {
            session->streaming = false;
            bool anyOtherStreaming = false;
            for (const ClientSession *c : std::as_const(m_clients)) {
                if (c != session && c->streaming) {
                    anyOtherStreaming = true;
                    break;
                }
            }
            if (!anyOtherStreaming && !m_keepCameraOpen && !m_videoThread->isRunning()) {
                m_camera->stop();
            }
            ok = true;
        } else if (cmd == "get_state" || cmd == "ping") {
            updateCachedTelemetry();
            ok = true;
        } else if (cmd == "set_stream_config") {
            if (params.contains("jpeg_quality")) {
                session->jpegQuality = std::clamp(params.value("jpeg_quality").toInt(m_jpegQuality), 0, 100);
                m_jpegQuality = session->jpegQuality;
            }
            if (params.contains("stream_enabled")) {
                session->streaming = params.value("stream_enabled").toBool(true);
            }
            ok = true;
        } else if (cmd == "set_auto_exposure") {
            ok = m_camera->setAutoExposure(params.value("enabled").toBool(true));
            updateCachedTelemetry();
        } else if (cmd == "set_exposure_time") {
            ok = m_camera->setExposureTime(params.value("exposure_time_ms").toDouble(10.0));
            updateCachedTelemetry();
        } else if (cmd == "set_analog_gain") {
            ok = m_camera->setAnalogGain(params.value("gain").toInt(1));
            updateCachedTelemetry();
        } else if (cmd == "set_ae_target") {
            ok = m_camera->setAeTarget(params.value("target").toInt(120));
            updateCachedTelemetry();
        } else if (cmd == "set_roi") {
            ok = m_camera->setRoi(params.value("enable").toBool(false));
            updateCachedTelemetry();
        } else if (cmd == "set_trigger_mode") {
            ok = m_camera->setTriggerMode(params.value("mode").toInt(0));
        } else if (cmd == "set_trigger_count") {
            ok = m_camera->setTriggerCount(params.value("count").toInt(1));
        } else if (cmd == "set_trigger_delay") {
            ok = m_camera->setTriggerDelay(params.value("delay_us").toInt(0));
        } else if (cmd == "set_trigger_interval") {
            ok = m_camera->setTriggerInterval(params.value("interval_us").toInt(0));
        } else if (cmd == "set_ext_trig_signal_type") {
            ok = m_camera->setExternalTriggerSignalType(params.value("type").toInt(0));
        } else if (cmd == "set_ext_trig_jitter_time") {
            ok = m_camera->setExternalTriggerJitterTime(params.value("time_us").toInt(0));
        } else if (cmd == "set_ext_trig_shutter_mode") {
            ok = m_camera->setExternalTriggerShutterMode(params.value("mode").toInt(0));
        } else if (cmd == "set_strobe_mode") {
            ok = m_camera->setStrobeMode(params.value("mode").toInt(0));
        } else if (cmd == "set_strobe_polarity") {
            ok = m_camera->setStrobePolarity(params.value("polarity").toInt(0));
        } else if (cmd == "set_strobe_delay_time") {
            ok = m_camera->setStrobeDelayTime(params.value("delay_us").toInt(0));
        } else if (cmd == "set_strobe_pulse_width") {
            ok = m_camera->setStrobePulseWidth(params.value("width_us").toInt(0));
        } else if (cmd == "trigger_software") {
            ok = m_camera->triggerSoftware();
        } else if (cmd == "start_recording") {
            int width = params.value("width").toInt(m_lastFrameWidth > 0 ? m_lastFrameWidth : 1280);
            int height = params.value("height").toInt(m_lastFrameHeight > 0 ? m_lastFrameHeight : 1024);
            double fps = params.value("fps").toDouble(m_lastFps > 0.1 ? m_lastFps : 30.0);
            QString filename = params.value("filename").toString().trimmed();
            if (filename.isEmpty()) {
                QDir dir(m_recordingDir);
                dir.mkpath(".");
                filename = dir.filePath(
                    QString("remote_recording_%1.mkv").arg(QDateTime::currentSecsSinceEpoch()));
            } else {
                QFileInfo fi(filename);
                if (fi.isRelative()) {
                    QDir dir(m_recordingDir);
                    dir.mkpath(".");
                    filename = dir.filePath(filename);
                } else {
                    fi.dir().mkpath(".");
                }
            }
            if (!m_videoThread->isRunning()) {
                m_videoThread->startRecording(width, height, fps, filename);
                m_lastRecordingFile = QFileInfo(filename).absoluteFilePath();
                emit q->logMessage(QString("Server recording started: %1").arg(m_lastRecordingFile));
            }
            extraResult["filename"] = m_lastRecordingFile;
            ok = true;
        } else if (cmd == "stop_recording") {
            if (m_videoThread->isRunning()) {
                m_videoThread->stopRecording();
                emit q->logMessage(QString("Server recording stopped: %1").arg(m_lastRecordingFile));
            }
            extraResult["filename"] = m_lastRecordingFile;
            ok = true;
        } else {
            ok = false;
            errorMsg = QString("Unknown command: %1").arg(cmd);
        }

        QJsonObject resultObj = buildCameraStateJson();
        for (auto it = extraResult.begin(); it != extraResult.end(); ++it) {
            resultObj[it.key()] = it.value();
        }

        QJsonObject resp;
        resp["id"] = reqId;
        resp["ok"] = ok;
        resp["result"] = resultObj;
        if (!errorMsg.isEmpty()) {
            resp["error"] = errorMsg;
        }

        if (session->socket && session->socket->state() == QAbstractSocket::ConnectedState) {
            session->socket->write(CameraNet::packJsonPacket(resp));
            session->socket->flush();
        }
    }

    void onCameraFrameReady(QImage image, qint64 timestampMs)
    {
        if (image.isNull()) {
            return;
        }

        m_lastFrameWidth = image.width();
        m_lastFrameHeight = image.height();

        if (m_videoThread->isRunning()) {
            m_videoThread->addFrame(image);
        }

        bool anyReadyClient = false;
        int targetQuality = m_jpegQuality;
        for (const ClientSession *c : std::as_const(m_clients)) {
            if (c->streaming && c->socket &&
                c->socket->state() == QAbstractSocket::ConnectedState &&
                c->socket->bytesToWrite() <= kMaxPendingSocketBytes) {
                anyReadyClient = true;
                targetQuality = c->jpegQuality;
                break;
            }
        }

        if (!anyReadyClient) {
            return;
        }

        CameraNet::FramePacketHeader hdr;
        hdr.timestampMs = timestampMs;
        hdr.width = static_cast<quint32>(image.width());
        hdr.height = static_cast<quint32>(image.height());
        hdr.exposureTimeMs = m_cachedExposureMs;
        hdr.analogGain = m_cachedAnalogGain;
        hdr.autoExposure = m_cachedAutoExposure;
        hdr.remoteRecording = m_videoThread->isRunning();
        hdr.aeTarget = static_cast<quint8>(std::clamp(m_cachedAeTarget, 0, 255));

        m_encoder->submitFrame(image, hdr, targetQuality);
    }

    void broadcastFramePacket(QByteArray packet)
    {
        for (ClientSession *c : std::as_const(m_clients)) {
            if (!c->streaming || !c->socket || c->socket->state() != QAbstractSocket::ConnectedState) {
                continue;
            }
            if (c->socket->bytesToWrite() <= kMaxPendingSocketBytes) {
                c->socket->write(packet);
            }
        }
    }

    void onCameraFpsChanged(double fps)
    {
        m_lastFps = fps;
        QJsonObject ev;
        ev["event"] = "fps";
        ev["fps"] = fps;
        broadcastJsonEvent(ev);
    }

    void onCameraQueueStatsChanged(qulonglong queueSize, qulonglong droppedFrames)
    {
        updateCachedTelemetry();
        QJsonObject ev = buildCameraStateJson();
        ev["event"] = "stats";
        ev["queue_size"] = static_cast<qint64>(queueSize);
        ev["dropped_frames"] = static_cast<qint64>(droppedFrames);
        broadcastJsonEvent(ev);
    }

    void onCameraError(QString message)
    {
        emit q->errorOccurred(message);
        QJsonObject ev;
        ev["event"] = "error";
        ev["message"] = message;
        broadcastJsonEvent(ev);
    }

public:
    MindVisionCameraServer *q;
    MindVisionCamera *m_camera;
    VideoThread *m_videoThread;
    QTcpServer *m_tcpServer;
    QTimer *m_telemetryTimer;
    QThread *m_encoderThread;
    ServerFrameEncoder *m_encoder;

    QList<ClientSession *> m_clients;
    QString m_bindAddress = QStringLiteral("0.0.0.0");
    quint16 m_port = CameraNet::kDefaultServerPort;
    int m_jpegQuality = 85;
    QString m_recordingDir = QStringLiteral("videos");
    QString m_lastRecordingFile;
    bool m_keepCameraOpen = false;

    int m_lastFrameWidth = 0;
    int m_lastFrameHeight = 0;
    double m_lastFps = 0.0;

    bool m_cachedAutoExposure = true;
    double m_cachedExposureMs = 15.0;
    int m_cachedAnalogGain = 1;
    int m_cachedAeTarget = 120;
};

// =============================================================================
// MindVisionCameraServer Implementation
// =============================================================================

MindVisionCameraServer::MindVisionCameraServer(QObject *parent)
    : QObject(parent), d(new MindVisionCameraServerPrivate(this))
{
}

MindVisionCameraServer::~MindVisionCameraServer()
{
    delete d;
}

bool MindVisionCameraServer::startServer(const QString &bindAddress, quint16 port)
{
    if (d->m_tcpServer->isListening()) {
        d->stopServer();
    }

    d->m_bindAddress = bindAddress.trimmed().isEmpty() ? QStringLiteral("0.0.0.0") : bindAddress.trimmed();
    QHostAddress addr;
    if (d->m_bindAddress == "0.0.0.0" || d->m_bindAddress.compare("any", Qt::CaseInsensitive) == 0) {
        addr = QHostAddress::Any;
    } else if (d->m_bindAddress.compare("localhost", Qt::CaseInsensitive) == 0) {
        addr = QHostAddress::LocalHost;
    } else if (!addr.setAddress(d->m_bindAddress)) {
        emit errorOccurred(QString("Invalid bind address: %1").arg(d->m_bindAddress));
        return false;
    }

    if (!d->m_tcpServer->listen(addr, port)) {
        const QString err = QString("Failed to listen on %1:%2 (%3)")
                                .arg(d->m_bindAddress)
                                .arg(port)
                                .arg(d->m_tcpServer->errorString());
        emit errorOccurred(err);
        return false;
    }

    d->m_port = d->m_tcpServer->serverPort();
    emit logMessage(QString("MindVisionCameraServer listening on %1:%2")
                        .arg(d->m_bindAddress)
                        .arg(d->m_port));
    return true;
}

void MindVisionCameraServer::stopServer()
{
    d->stopServer();
}

bool MindVisionCameraServer::isListening() const
{
    return d->m_tcpServer->isListening();
}

QString MindVisionCameraServer::bindAddress() const
{
    return d->m_bindAddress;
}

quint16 MindVisionCameraServer::serverPort() const
{
    return d->m_tcpServer->isListening() ? d->m_tcpServer->serverPort() : d->m_port;
}

int MindVisionCameraServer::connectedClientCount() const
{
    return d->m_clients.size();
}

void MindVisionCameraServer::setSyntheticMode(bool enabled)
{
    d->m_camera->setSyntheticMode(enabled);
}

bool MindVisionCameraServer::isSyntheticMode() const
{
    return d->m_camera->isSyntheticMode();
}

void MindVisionCameraServer::setJpegQuality(int quality)
{
    d->m_jpegQuality = std::clamp(quality, 0, 100);
}

int MindVisionCameraServer::jpegQuality() const
{
    return d->m_jpegQuality;
}

void MindVisionCameraServer::setRecordingDirectory(const QString &dirPath)
{
    if (!dirPath.trimmed().isEmpty()) {
        d->m_recordingDir = dirPath.trimmed();
    }
}

QString MindVisionCameraServer::recordingDirectory() const
{
    return d->m_recordingDir;
}

void MindVisionCameraServer::setKeepCameraOpen(bool keepOpen)
{
    d->m_keepCameraOpen = keepOpen;
}

bool MindVisionCameraServer::keepCameraOpen() const
{
    return d->m_keepCameraOpen;
}

bool MindVisionCameraServer::openCamera()
{
    const bool ok = d->m_camera->open();
    if (ok) {
        d->updateCachedTelemetry();
    }
    return ok;
}

void MindVisionCameraServer::closeCamera()
{
    d->m_camera->close();
}

bool MindVisionCameraServer::startCapture()
{
    if (!d->m_camera->isOpen() && !d->m_camera->open()) {
        return false;
    }
    const bool ok = d->m_camera->start();
    if (ok) {
        d->updateCachedTelemetry();
    }
    return ok;
}

void MindVisionCameraServer::stopCapture()
{
    d->m_camera->stop();
}

MindVisionCamera *MindVisionCameraServer::camera() const
{
    return d->m_camera;
}

#include "MindVisionCameraServer.moc"
