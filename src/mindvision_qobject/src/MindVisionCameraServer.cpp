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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QRegularExpression>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
// Maximum queued bytes per client socket before dropping preview frames for that client
constexpr qint64 kMaxPendingSocketBytes = 2 * 1024 * 1024;
constexpr double kMinimumExposureMs = 0.001;

std::string mcpJsonResult(bool ok, const QString &message,
                          const QJsonObject &extra = QJsonObject())
{
    QJsonObject obj = extra;
    obj["ok"] = ok;
    obj["message"] = message;
    return QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
}

QString normalizeStageRx(const QString &content)
{
    QString result;
    result.reserve(content.size());
    for (const QChar &ch : content) {
        if (ch >= ' ' && ch <= '~') {
            result.append(ch);
        }
    }
    return result.trimmed();
}
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
    bool stageSubscribed = false;
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
          m_stagePollTimer(new QTimer(this)),
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

        connect(&m_stageSerial, &QSerialPort::readyRead,
                this, &MindVisionCameraServerPrivate::onStageReadyRead);
        connect(&m_stageSerial, &QSerialPort::errorOccurred,
                this, &MindVisionCameraServerPrivate::onStageSerialError);

        connect(m_telemetryTimer, &QTimer::timeout,
                this, &MindVisionCameraServerPrivate::updateCachedTelemetry);
        m_telemetryTimer->start(200);

        connect(m_stagePollTimer, &QTimer::timeout,
                this, &MindVisionCameraServerPrivate::pollStageStatus);
    }

    ~MindVisionCameraServerPrivate() override
    {
        stopServer();
        disconnectStage();
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

    QStringList availableStagePorts() const
    {
        QStringList list;
        const auto infos = QSerialPortInfo::availablePorts();
        for (const QSerialPortInfo &info : infos) {
            if (!info.portName().isEmpty() && !list.contains(info.portName())) {
                list.append(info.portName());
            }
        }
        if (m_camera->isSyntheticMode() || m_stageSyntheticConnected) {
            if (!list.contains(QStringLiteral("SYNTHETIC_STAGE"))) {
                list.prepend(QStringLiteral("SYNTHETIC_STAGE"));
            }
        }
        return list;
    }

    bool isStageConnected() const
    {
        return m_stageSerial.isOpen() || m_stageSyntheticConnected;
    }

    QJsonObject buildStageStateJson() const
    {
        QJsonObject st;
        st["connected"] = isStageConnected();
        st["port_name"] = m_stagePortName;
        st["baud_rate"] = m_stageBaudRate;
        st["state"] = isStageConnected() ? m_stageState : QStringLiteral("Disconnected");
        st["has_position"] = m_hasStagePosition;
        st["position_mm"] = QJsonObject{
            {"x", m_stageX},
            {"y", m_stageY},
            {"z", m_stageZ}
        };
        QJsonArray portsArr;
        for (const QString &p : availableStagePorts()) {
            portsArr.append(p);
        }
        st["ports"] = portsArr;
        return st;
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
        obj["stage"] = buildStageStateJson();
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

    void broadcastStageEvent(const QJsonObject &eventObj)
    {
        if (m_clients.isEmpty()) {
            return;
        }
        const QByteArray packet = CameraNet::packJsonPacket(eventObj);
        for (ClientSession *session : std::as_const(m_clients)) {
            if (session->stageSubscribed &&
                session->socket &&
                session->socket->state() == QAbstractSocket::ConnectedState) {
                session->socket->write(packet);
                session->socket->flush();
            }
        }
    }

    void observeStageRxLine(const QString &rxContent)
    {
        const QString normalized = normalizeStageRx(rxContent);
        if (normalized.isEmpty()) {
            return;
        }
        if (normalized.startsWith('<') && normalized.endsWith('>')) {
            const QString inner = normalized.mid(1, normalized.size() - 2);
            const QStringList parts = inner.split('|');
            if (!parts.isEmpty() && !parts[0].isEmpty()) {
                m_stageState = parts[0];
            }
            static const QRegularExpression posRe(
                QStringLiteral("(?:WPos|MPos):(-?[\\d\\.]+),(-?[\\d\\.]+),(-?[\\d\\.]+)"));
            const auto match = posRe.match(inner);
            if (match.hasMatch()) {
                m_stageX = match.captured(1).toDouble();
                m_stageY = match.captured(2).toDouble();
                m_stageZ = match.captured(3).toDouble();
                m_hasStagePosition = true;
            }
            return;
        }

        const QString lowered = normalized.toLower();
        static const QRegularExpression okRe(QStringLiteral("(^|[^a-z])ok([^a-z]|$)"));
        if (okRe.match(lowered).hasMatch() || lowered.contains(QStringLiteral("error"))) {
            if (m_mcpStageWaitingForOk) {
                m_mcpStageWaitingForOk = false;
                pumpMcpStageQueue();
            }
        }
    }

    void enqueueMcpStageCommand(const QString &cmd)
    {
        m_mcpStageQueue.append(cmd);
        pumpMcpStageQueue();
    }

    void pumpMcpStageQueue()
    {
        if (m_pumpingMcpStageQueue) {
            return;
        }
        m_pumpingMcpStageQueue = true;
        while (!m_mcpStageWaitingForOk && !m_mcpStageQueue.isEmpty()) {
            const QString cmd = m_mcpStageQueue.takeFirst();
            m_mcpStageWaitingForOk = true;
            if (!sendStageCommand(cmd)) {
                m_mcpStageWaitingForOk = false;
                m_mcpStageQueue.clear();
                break;
            }
        }
        m_pumpingMcpStageQueue = false;
    }

    void emitAndBroadcastStageLog(const QString &msg)
    {
        if (msg.startsWith(QStringLiteral("Rx: "))) {
            observeStageRxLine(msg.mid(4));
        }
        emit q->stageLogMessage(msg);
        QJsonObject ev;
        ev["event"] = "stage_log";
        ev["message"] = msg;
        broadcastStageEvent(ev);
    }

    void emitAndBroadcastStageStatus()
    {
        const bool connected = isStageConnected();
        emit q->stageStatusChanged(connected, m_stagePortName);
        QJsonObject ev;
        ev["event"] = "stage_status";
        ev["connected"] = connected;
        ev["port_name"] = m_stagePortName;
        ev["baud_rate"] = m_stageBaudRate;
        broadcastStageEvent(ev);
    }

    bool connectStage(const QString &requestedPort, int baudRate, QString *errorOut = nullptr)
    {
        QString portName = requestedPort.trimmed();
        if (portName.isEmpty() && isStageConnected()) {
            // Attach to already-connected stage port
            emitAndBroadcastStageStatus();
            return true;
        }
        if (portName.isEmpty()) {
            const QStringList ports = availableStagePorts();
            if (!ports.isEmpty()) {
                portName = ports.first();
            }
        }
        if (portName.isEmpty()) {
            const QString err = QStringLiteral("No stage serial port specified or available on server.");
            if (errorOut) *errorOut = err;
            emitAndBroadcastStageLog(QStringLiteral("Failed to connect stage: ") + err);
            return false;
        }

        // If already connected to the exact same port & baud rate, avoid reopening/resetting DTR
        if (isStageConnected() && m_stagePortName == portName && m_stageBaudRate == baudRate) {
            emitAndBroadcastStageLog(
                QString("Connected to %1 at %2 baud.").arg(portName).arg(baudRate));
            emitAndBroadcastStageStatus();
            return true;
        }

        disconnectStage();

        if (portName.compare(QStringLiteral("SYNTHETIC_STAGE"), Qt::CaseInsensitive) == 0) {
            m_stageSyntheticConnected = true;
            m_stagePortName = QStringLiteral("SYNTHETIC_STAGE");
            m_stageBaudRate = baudRate > 0 ? baudRate : 115200;
            m_synStageRelative = false;
            m_synStageState = QStringLiteral("Idle");
            m_stageState = QStringLiteral("Idle");
            m_stageX = m_synStageX;
            m_stageY = m_synStageY;
            m_stageZ = m_synStageZ;
            m_hasStagePosition = true;
            emit q->logMessage(QString("Stage connected to SYNTHETIC_STAGE (%1 baud)").arg(m_stageBaudRate));
            emitAndBroadcastStageLog(
                QString("Connected to %1 at %2 baud.").arg(m_stagePortName).arg(m_stageBaudRate));
            emitAndBroadcastStageStatus();
            emitAndBroadcastStageLog(QStringLiteral("Rx: Grbl 4.0 [FluidNC synthetic]"));
            emitAndBroadcastStageLog(formatSyntheticStageStatus());
            return true;
        }

        m_stageSerial.setPortName(portName);
        m_stageSerial.setBaudRate(baudRate > 0 ? baudRate : 115200);
        m_stageSerial.setDataBits(QSerialPort::Data8);
        m_stageSerial.setParity(QSerialPort::NoParity);
        m_stageSerial.setStopBits(QSerialPort::OneStop);
        m_stageSerial.setFlowControl(QSerialPort::NoFlowControl);

        if (!m_stageSerial.open(QIODevice::ReadWrite)) {
            const QString err = QString("Failed to connect to %1: %2")
                                    .arg(portName, m_stageSerial.errorString());
            if (errorOut) *errorOut = err;
            emit q->errorOccurred(err);
            emitAndBroadcastStageLog(err);
            emitAndBroadcastStageStatus();
            return false;
        }

        m_stagePortName = portName;
        m_stageBaudRate = baudRate > 0 ? baudRate : 115200;
        m_stageReadBuffer.clear();
        m_stageState = QStringLiteral("Unknown");
        m_hasStagePosition = false;
        m_stagePollTimer->start(250);
        emit q->logMessage(QString("Stage serial connected: %1 @ %2 baud")
                               .arg(m_stagePortName)
                               .arg(m_stageBaudRate));
        emitAndBroadcastStageLog(
            QString("Connected to %1 at %2 baud.").arg(m_stagePortName).arg(m_stageBaudRate));
        emitAndBroadcastStageStatus();
        sendStageRawCommand(QByteArrayLiteral("?"));
        return true;
    }

    void disconnectStage()
    {
        const bool wasConnected = isStageConnected();
        m_stagePollTimer->stop();
        m_mcpStageQueue.clear();
        m_mcpStageWaitingForOk = false;
        if (m_stageSerial.isOpen()) {
            m_stageSerial.close();
        }
        m_stageSyntheticConnected = false;
        m_stageReadBuffer.clear();
        m_stageState = QStringLiteral("Disconnected");
        m_hasStagePosition = false;
        if (wasConnected) {
            emit q->logMessage(QString("Stage serial disconnected (%1).").arg(m_stagePortName));
            emitAndBroadcastStageLog(QStringLiteral("Disconnected from serial port."));
            emitAndBroadcastStageStatus();
        }
    }

    QString formatSyntheticStageStatus() const
    {
        return QString("Rx: <%1|WPos:%2,%3,%4>")
            .arg(m_synStageState)
            .arg(m_synStageX, 0, 'f', 3)
            .arg(m_synStageY, 0, 'f', 3)
            .arg(m_synStageZ, 0, 'f', 3);
    }

    void handleSyntheticStageCommand(const QString &rawCmd)
    {
        const QString cmd = rawCmd.trimmed();
        if (cmd.isEmpty()) {
            emitAndBroadcastStageLog(QStringLiteral("Rx: ok"));
            return;
        }
        if (cmd.startsWith(QStringLiteral("__SCAN_"))) {
            emitAndBroadcastStageLog(QStringLiteral("Rx: ok"));
            return;
        }
        const QString upper = cmd.toUpper();
        if (upper == QStringLiteral("$H")) {
            m_synStageX = 0.0;
            m_synStageY = 0.0;
            m_synStageZ = 0.0;
            m_synStageState = QStringLiteral("Idle");
            emitAndBroadcastStageLog(QStringLiteral("Rx: ok"));
            emitAndBroadcastStageLog(formatSyntheticStageStatus());
            return;
        }
        if (upper == QStringLiteral("$HX")) {
            m_synStageX = 0.0;
            m_synStageState = QStringLiteral("Idle");
            emitAndBroadcastStageLog(QStringLiteral("Rx: ok"));
            emitAndBroadcastStageLog(formatSyntheticStageStatus());
            return;
        }
        if (upper == QStringLiteral("$HY")) {
            m_synStageY = 0.0;
            m_synStageState = QStringLiteral("Idle");
            emitAndBroadcastStageLog(QStringLiteral("Rx: ok"));
            emitAndBroadcastStageLog(formatSyntheticStageStatus());
            return;
        }
        if (upper == QStringLiteral("!")) {
            m_synStageState = QStringLiteral("Hold:0");
            emitAndBroadcastStageLog(QStringLiteral("Rx: ok"));
            emitAndBroadcastStageLog(formatSyntheticStageStatus());
            return;
        }
        if (upper == QStringLiteral("~")) {
            m_synStageState = QStringLiteral("Idle");
            emitAndBroadcastStageLog(QStringLiteral("Rx: ok"));
            emitAndBroadcastStageLog(formatSyntheticStageStatus());
            return;
        }

        bool localRelative = m_synStageRelative;
        if (upper.contains(QStringLiteral("G90"))) {
            localRelative = false;
            if (!upper.startsWith(QStringLiteral("$J="))) {
                m_synStageRelative = false;
            }
        }
        if (upper.contains(QStringLiteral("G91"))) {
            localRelative = true;
            if (!upper.startsWith(QStringLiteral("$J="))) {
                m_synStageRelative = true;
            }
        }

        static const QRegularExpression axisRe(QStringLiteral("([XYZ])\\s*(-?\\d+(?:\\.\\d+)?)"));
        auto it = axisRe.globalMatch(upper);
        while (it.hasNext()) {
            const auto match = it.next();
            const QString axis = match.captured(1);
            const double val = match.captured(2).toDouble();
            if (axis == QStringLiteral("X")) {
                m_synStageX = localRelative ? (m_synStageX + val) : val;
            } else if (axis == QStringLiteral("Y")) {
                m_synStageY = localRelative ? (m_synStageY + val) : val;
            } else if (axis == QStringLiteral("Z")) {
                m_synStageZ = localRelative ? (m_synStageZ + val) : val;
            }
        }
        m_synStageState = QStringLiteral("Idle");
        emitAndBroadcastStageLog(QStringLiteral("Rx: ok"));
        emitAndBroadcastStageLog(formatSyntheticStageStatus());
    }

    bool sendStageCommand(const QString &cmd, QString *errorOut = nullptr)
    {
        if (!isStageConnected()) {
            const QString err = QStringLiteral("Error: Serial port not open.");
            if (errorOut) *errorOut = err;
            emitAndBroadcastStageLog(err);
            return false;
        }

        if (m_stageSyntheticConnected) {
            handleSyntheticStageCommand(cmd);
            return true;
        }

        const QByteArray fullCmd = (cmd + QStringLiteral("\n")).toLatin1();
        const qint64 written = m_stageSerial.write(fullCmd);
        if (written != fullCmd.size()) {
            const QString err = written < 0
                ? m_stageSerial.errorString()
                : QString("short write (%1/%2 bytes)").arg(written).arg(fullCmd.size());
            const QString fullErr = QString("Error sending command '%1': %2").arg(cmd, err);
            if (errorOut) *errorOut = fullErr;
            emitAndBroadcastStageLog(fullErr);
            return false;
        }
        m_stageSerial.flush();
        return true;
    }

    bool sendStageRawCommand(const QByteArray &rawCmd, QString *errorOut = nullptr)
    {
        if (!isStageConnected()) {
            if (errorOut) *errorOut = QStringLiteral("Serial port not open.");
            return false;
        }

        if (m_stageSyntheticConnected) {
            if (rawCmd.contains('?')) {
                emitAndBroadcastStageLog(formatSyntheticStageStatus());
            }
            if (rawCmd.contains('\x18')) {
                m_synStageState = QStringLiteral("Idle");
                emitAndBroadcastStageLog(QStringLiteral("Rx: Grbl 4.0 [FluidNC synthetic]"));
                emitAndBroadcastStageLog(formatSyntheticStageStatus());
            }
            if (rawCmd.contains('!')) {
                m_synStageState = QStringLiteral("Hold:0");
                emitAndBroadcastStageLog(formatSyntheticStageStatus());
            }
            if (rawCmd.contains('~')) {
                m_synStageState = QStringLiteral("Idle");
                emitAndBroadcastStageLog(formatSyntheticStageStatus());
            }
            return true;
        }

        const qint64 written = m_stageSerial.write(rawCmd);
        if (written != rawCmd.size()) {
            if (errorOut) *errorOut = m_stageSerial.errorString();
            return false;
        }
        m_stageSerial.flush();
        return true;
    }

public slots:
    void onStageReadyRead()
    {
        m_stageReadBuffer.append(m_stageSerial.readAll());
        while (true) {
            const int nlIdx = m_stageReadBuffer.indexOf('\n');
            if (nlIdx < 0) {
                break;
            }
            const QByteArray line = m_stageReadBuffer.left(nlIdx + 1);
            m_stageReadBuffer.remove(0, nlIdx + 1);
            const QString s = QString::fromLatin1(line).trimmed();
            if (!s.isEmpty()) {
                emitAndBroadcastStageLog(QStringLiteral("Rx: ") + s);
            }
        }
    }

    void onStageSerialError(QSerialPort::SerialPortError error)
    {
        if (error == QSerialPort::ResourceError) {
            const QString err = QString("Serial port resource error: %1")
                                    .arg(m_stageSerial.errorString());
            emit q->errorOccurred(err);
            emitAndBroadcastStageLog(err);
            disconnectStage();
        }
    }

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

    void pollStageStatus()
    {
        if (!m_stageSerial.isOpen()) {
            return;
        }
        // When a remote GUI CNCControlPanel client is subscribed, it already polls '?'.
        for (const ClientSession *c : std::as_const(m_clients)) {
            if (c->stageSubscribed &&
                c->socket &&
                c->socket->state() == QAbstractSocket::ConnectedState) {
                return;
            }
        }
        sendStageRawCommand(QByteArrayLiteral("?"));
    }

    void broadcastCameraStats()
    {
        updateCachedTelemetry();
        QJsonObject ev = buildCameraStateJson();
        ev["event"] = "stats";
        broadcastJsonEvent(ev);
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
        } else if (cmd == "stage_subscribe") {
            session->stageSubscribed = params.value("enabled").toBool(true);
            extraResult["stage"] = buildStageStateJson();
            ok = true;
        } else if (cmd == "stage_list_ports") {
            extraResult["stage"] = buildStageStateJson();
            ok = true;
        } else if (cmd == "stage_connect") {
            session->stageSubscribed = true;
            const QString portName = params.value("port_name").toString().trimmed();
            const int baudRate = params.value("baud_rate").toInt(115200);
            ok = connectStage(portName, baudRate, &errorMsg);
            extraResult["stage"] = buildStageStateJson();
        } else if (cmd == "stage_disconnect") {
            session->stageSubscribed = true;
            disconnectStage();
            extraResult["stage"] = buildStageStateJson();
            ok = true;
        } else if (cmd == "stage_send_command") {
            session->stageSubscribed = true;
            const QString stageCmd = params.value("command").toString();
            ok = sendStageCommand(stageCmd, &errorMsg);
            extraResult["stage"] = buildStageStateJson();
        } else if (cmd == "stage_send_raw") {
            session->stageSubscribed = true;
            QByteArray rawBytes;
            if (params.contains("data_hex")) {
                rawBytes = QByteArray::fromHex(params.value("data_hex").toString().toLatin1());
            } else {
                rawBytes = params.value("command").toString().toLatin1();
            }
            ok = sendStageRawCommand(rawBytes, &errorMsg);
            extraResult["stage"] = buildStageStateJson();
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

        m_latestFrame = image;
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
    QTimer *m_stagePollTimer;
    QThread *m_encoderThread;
    ServerFrameEncoder *m_encoder;

    QList<ClientSession *> m_clients;
    QString m_bindAddress = QStringLiteral("0.0.0.0");
    quint16 m_port = CameraNet::kDefaultServerPort;
    int m_jpegQuality = 85;
    QString m_recordingDir = QStringLiteral("videos");
    QString m_lastRecordingFile;
    bool m_keepCameraOpen = false;

    QImage m_latestFrame;
    int m_lastFrameWidth = 0;
    int m_lastFrameHeight = 0;
    double m_lastFps = 0.0;

    bool m_cachedAutoExposure = true;
    double m_cachedExposureMs = 15.0;
    int m_cachedAnalogGain = 1;
    int m_cachedAeTarget = 120;

    // Stage serial controller state
    QSerialPort m_stageSerial;
    QByteArray m_stageReadBuffer;
    QString m_stagePortName;
    int m_stageBaudRate = 115200;
    QString m_stageState = QStringLiteral("Disconnected");
    double m_stageX = 0.0;
    double m_stageY = 0.0;
    double m_stageZ = 0.0;
    bool m_hasStagePosition = false;
    QStringList m_mcpStageQueue;
    bool m_mcpStageWaitingForOk = false;
    bool m_pumpingMcpStageQueue = false;
    bool m_stageSyntheticConnected = false;
    bool m_synStageRelative = false;
    double m_synStageX = 0.0;
    double m_synStageY = 0.0;
    double m_synStageZ = 0.0;
    QString m_synStageState = QStringLiteral("Idle");
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
    d->m_latestFrame = QImage();
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

QStringList MindVisionCameraServer::availableStagePorts() const
{
    return d->availableStagePorts();
}

bool MindVisionCameraServer::connectStage(const QString &portName, int baudRate)
{
    return d->connectStage(portName, baudRate);
}

void MindVisionCameraServer::disconnectStage()
{
    d->disconnectStage();
}

bool MindVisionCameraServer::isStageConnected() const
{
    return d->isStageConnected();
}

QString MindVisionCameraServer::stagePortName() const
{
    return d->m_stagePortName;
}

int MindVisionCameraServer::stageBaudRate() const
{
    return d->m_stageBaudRate;
}

bool MindVisionCameraServer::sendStageCommand(const QString &cmd)
{
    return d->sendStageCommand(cmd);
}

bool MindVisionCameraServer::sendStageRawCommand(const QByteArray &rawCmd)
{
    return d->sendStageRawCommand(rawCmd);
}

MindVisionCamera *MindVisionCameraServer::camera() const
{
    return d->m_camera;
}

// =============================================================================
// Headless Hardware MCP Entry Points
// =============================================================================

std::string MindVisionCameraServer::mcpStatusJson() const
{
    d->updateCachedTelemetry();

    QJsonObject result;
    result["role"] = QStringLiteral("hardware");
    result["camera_open"] = d->m_camera->isOpen();
    result["camera_running"] = d->m_camera->isRunning();
    result["synthetic_mode"] = d->m_camera->isSyntheticMode();
    result["recording"] = d->m_videoThread->isRunning();
    result["last_recording_file"] = d->m_lastRecordingFile;
    result["fps"] = d->m_lastFps;
    result["connected_clients"] = d->m_clients.size();
    result["tcp_bind_address"] = d->m_bindAddress;
    result["tcp_port"] = static_cast<int>(serverPort());

    if (d->m_camera->isOpen()) {
        result["auto_exposure"] = d->m_cachedAutoExposure;
        result["exposure_ms"] = d->m_cachedExposureMs;
        result["gain"] = d->m_cachedAnalogGain;
    }
    if (d->m_lastFrameWidth > 0 && d->m_lastFrameHeight > 0) {
        result["width"] = d->m_lastFrameWidth;
        result["height"] = d->m_lastFrameHeight;
    }

    result["stage_connected"] = d->isStageConnected();
    result["stage_port"] = d->m_stagePortName;
    QJsonArray portsArr;
    for (const QString &p : d->availableStagePorts()) {
        portsArr.append(p);
    }
    result["available_stage_ports"] = portsArr;
    result["cnc_state"] = d->isStageConnected() ? d->m_stageState : QStringLiteral("Disconnected");
    result["stage_position_mm"] = QJsonObject{
        {"x", d->m_stageX},
        {"y", d->m_stageY},
        {"z", d->m_stageZ}
    };
    return mcpJsonResult(true, QStringLiteral("Microscope hardware status retrieved."), result);
}

std::string MindVisionCameraServer::mcpStartCameraJson()
{
    if (!d->m_camera->isRunning()) {
        startCapture();
    }
    const bool running = d->m_camera->isRunning();
    if (running) {
        d->broadcastCameraStats();
    }
    return mcpJsonResult(running,
                         running ? QStringLiteral("Camera is running.")
                                 : QStringLiteral("Camera could not be started."));
}

std::string MindVisionCameraServer::mcpStopCameraJson()
{
    if (d->m_videoThread->isRunning()) {
        d->m_videoThread->stopRecording();
    }
    if (d->m_camera->isRunning()) {
        stopCapture();
    }
    d->broadcastCameraStats();
    return mcpJsonResult(true, QStringLiteral("Camera is stopped."));
}

std::string MindVisionCameraServer::mcpSnapshotJson()
{
    if (d->m_latestFrame.isNull()) {
        return mcpJsonResult(false, QStringLiteral("No camera frame is available. Start the camera and wait for a frame."));
    }

    QDir snapDir(QStringLiteral("snapshots"));
    snapDir.mkpath(QStringLiteral("."));
    const QString filename = snapDir.filePath(
        QString("snapshot_%1.png").arg(QDateTime::currentMSecsSinceEpoch()));
    if (!d->m_latestFrame.save(filename)) {
        return mcpJsonResult(false, QStringLiteral("Could not save the snapshot."));
    }
    const QString absPath = QFileInfo(filename).absoluteFilePath();
    emit logMessage(QString("MCP snapshot saved: %1").arg(absPath));
    return mcpJsonResult(true, QStringLiteral("Snapshot saved."), QJsonObject{{"path", absPath}});
}

std::string MindVisionCameraServer::mcpSetRecordingJson(bool enabled)
{
    if (enabled && !d->m_camera->isRunning()) {
        return mcpJsonResult(false, QStringLiteral("Start the camera before starting recording."));
    }

    if (enabled) {
        if (!d->m_videoThread->isRunning()) {
            const int width = d->m_lastFrameWidth > 0 ? d->m_lastFrameWidth : 1280;
            const int height = d->m_lastFrameHeight > 0 ? d->m_lastFrameHeight : 1024;
            const double fps = d->m_lastFps > 0.1 ? d->m_lastFps : 30.0;
            QDir dir(d->m_recordingDir);
            dir.mkpath(QStringLiteral("."));
            const QString filename = dir.filePath(
                QString("recording_%1.mkv").arg(QDateTime::currentSecsSinceEpoch()));
            d->m_videoThread->startRecording(width, height, fps, filename);
            d->m_lastRecordingFile = QFileInfo(filename).absoluteFilePath();
            emit logMessage(QString("MCP recording started: %1").arg(d->m_lastRecordingFile));
            d->broadcastCameraStats();
        }
        return mcpJsonResult(true, QStringLiteral("Recording started."),
                             QJsonObject{{"path", d->m_lastRecordingFile}});
    }

    if (d->m_videoThread->isRunning()) {
        d->m_videoThread->stopRecording();
        emit logMessage(QString("MCP recording stopped: %1").arg(d->m_lastRecordingFile));
        d->broadcastCameraStats();
    }
    return mcpJsonResult(true, QStringLiteral("Recording stopped."),
                         QJsonObject{{"path", d->m_lastRecordingFile}});
}

std::string MindVisionCameraServer::mcpSetExposureJson(double milliseconds)
{
    if (!std::isfinite(milliseconds) || milliseconds < kMinimumExposureMs || milliseconds > 10000.0) {
        return mcpJsonResult(false, QStringLiteral("Exposure must be between 0.001 and 10000 milliseconds."));
    }
    if (!d->m_camera->isOpen() && !openCamera()) {
        return mcpJsonResult(false, QStringLiteral("Camera is not open and could not be opened."));
    }
    const bool ok = d->m_camera->setExposureTime(milliseconds);
    d->broadcastCameraStats();
    return mcpJsonResult(ok,
                         ok ? QStringLiteral("Exposure updated.")
                            : QStringLiteral("Failed to update exposure."),
                         QJsonObject{{"exposure_ms", d->m_cachedExposureMs}});
}

std::string MindVisionCameraServer::mcpSetGainJson(int gain)
{
    if (!d->m_camera->isOpen() && !openCamera()) {
        return mcpJsonResult(false, QStringLiteral("Camera is not open and could not be opened."));
    }
    int minGain = 1;
    int maxGain = 100;
    d->m_camera->getAnalogGainRange(minGain, maxGain);
    if (minGain >= maxGain) {
        minGain = 1;
        maxGain = 100;
    }
    if (gain < minGain || gain > maxGain) {
        return mcpJsonResult(false, QString("Gain must be between %1 and %2.").arg(minGain).arg(maxGain));
    }
    const bool ok = d->m_camera->setAnalogGain(gain);
    d->broadcastCameraStats();
    return mcpJsonResult(ok,
                         ok ? QStringLiteral("Gain updated.")
                            : QStringLiteral("Failed to update gain."),
                         QJsonObject{{"gain", d->m_cachedAnalogGain}});
}

std::string MindVisionCameraServer::mcpSetAutoExposureJson(bool enabled)
{
    if (!d->m_camera->isOpen() && !openCamera()) {
        return mcpJsonResult(false, QStringLiteral("Camera is not open and could not be opened."));
    }
    const bool ok = d->m_camera->setAutoExposure(enabled);
    d->broadcastCameraStats();
    return mcpJsonResult(ok,
                         ok ? (enabled ? QStringLiteral("Auto exposure enabled.")
                                       : QStringLiteral("Auto exposure disabled."))
                            : QStringLiteral("Failed to update auto exposure."));
}

std::string MindVisionCameraServer::mcpMoveStageJson(double xMm, double yMm, int feedrate)
{
    if (!std::isfinite(xMm) || !std::isfinite(yMm) || feedrate < 1 || feedrate > 5000) {
        return mcpJsonResult(false, QStringLiteral("Stage coordinates must be finite and feedrate must be 1 to 5000 mm/min."));
    }
    if (!d->isStageConnected()) {
        QString err;
        if (!d->connectStage(QString(), d->m_stageBaudRate, &err)) {
            return mcpJsonResult(false, err.isEmpty() ? QStringLiteral("Stage is not connected.") : err);
        }
    }
    d->enqueueMcpStageCommand(QStringLiteral("G90"));
    d->enqueueMcpStageCommand(
        QString("G1 X%1 Y%2 F%3").arg(xMm, 0, 'f', 3).arg(yMm, 0, 'f', 3).arg(feedrate));
    emit logMessage(QString("MCP stage move requested: X%1 Y%2 F%3").arg(xMm).arg(yMm).arg(feedrate));
    return mcpJsonResult(true, QStringLiteral("Stage move queued."),
                         QJsonObject{{"target_mm", QJsonObject{{"x", xMm}, {"y", yMm}}},
                                     {"feedrate_mm_min", feedrate}});
}

std::string MindVisionCameraServer::mcpEmergencyStopStageJson()
{
    d->m_mcpStageQueue.clear();
    d->m_mcpStageWaitingForOk = false;
    if (!d->isStageConnected()) {
        return mcpJsonResult(false, QStringLiteral("Stage is not connected."));
    }
    QString err;
    const bool ok = d->sendStageRawCommand(QByteArrayLiteral("!"), &err);
    emit logMessage(QStringLiteral("MCP emergency stop requested."));
    return mcpJsonResult(ok,
                         ok ? QStringLiteral("Stage stop requested.")
                            : (err.isEmpty() ? QStringLiteral("Failed to request stage stop.") : err));
}

#include "MindVisionCameraServer.moc"
