#include "MindVisionCamera.h"
#include "CameraNetworkProtocol.h"
#include "VideoThread.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTcpSocket>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

// SDK Header
#include "CameraApi.h"

namespace {
bool g_sdkInitialized = false;
QMutex g_sdkInitMutex;

void ensureSdkInitialized()
{
    QMutexLocker locker(&g_sdkInitMutex);
    if (!g_sdkInitialized) {
        CameraSdkInit(0);
        g_sdkInitialized = true;
    }
}
} // namespace

// =============================================================================
// CameraWorker Declaration & Definition (Local SDK + Synthetic Mode)
// =============================================================================

class CameraWorker : public QObject
{
    Q_OBJECT
public:
    explicit CameraWorker(CameraHandle handle, int width, int height, bool synthetic = false);
    ~CameraWorker();

    QImage takeLatestPreviewFrame(qint64 &timestampMs);
    void getPreviewStats(qulonglong &received, qulonglong &emitted, qulonglong &dropped);
    void setSyntheticParams(double exposureMs, int gain, bool autoExp, int aeTarget, int triggerMode);
    void triggerSyntheticSoftware();

public slots:
    void process();
    void stop();

signals:
    void frameReady(QImage image, qint64 timestampMs);
    void previewFrameAvailable();
    void queueStatsChanged(qulonglong queueSize, qulonglong droppedFrames);
    void fpsChanged(double fps);
    void finished();

private:
    QImage renderSyntheticFrame(int frameIndex);

    CameraHandle m_hCamera;
    int m_width;
    int m_height;
    bool m_synthetic;
    bool m_stopRequested;
    unsigned char *m_pRgbBuffer;
    tSdkFrameHead m_frameHead;
    QMutex m_previewMutex;
    QImage m_latestPreviewFrame;
    qint64 m_latestPreviewTimestampMs;
    qulonglong m_previewFramesReceived;
    qulonglong m_previewFramesEmitted;
    qulonglong m_previewFramesDropped;
    bool m_previewPending;

    QMutex m_synthMutex;
    double m_synthExposureMs = 15.0;
    int m_synthGain = 4;
    bool m_synthAutoExp = true;
    int m_synthAeTarget = 120;
    int m_synthTriggerMode = 0;
    int m_synthPendingTriggers = 0;
};

CameraWorker::CameraWorker(CameraHandle handle, int width, int height, bool synthetic)
    : m_hCamera(handle),
      m_width(width),
      m_height(height),
      m_synthetic(synthetic),
      m_stopRequested(false),
      m_pRgbBuffer(nullptr),
      m_latestPreviewTimestampMs(0),
      m_previewFramesReceived(0),
      m_previewFramesEmitted(0),
      m_previewFramesDropped(0),
      m_previewPending(false)
{
    if (!m_synthetic) {
        int size = m_width * m_height * 3;
        m_pRgbBuffer = static_cast<unsigned char *>(malloc(size));
    }
}

CameraWorker::~CameraWorker()
{
    if (m_pRgbBuffer) {
        free(m_pRgbBuffer);
        m_pRgbBuffer = nullptr;
    }
}

void CameraWorker::stop()
{
    m_stopRequested = true;
}

void CameraWorker::setSyntheticParams(double exposureMs, int gain, bool autoExp, int aeTarget, int triggerMode)
{
    QMutexLocker locker(&m_synthMutex);
    m_synthExposureMs = exposureMs;
    m_synthGain = gain;
    m_synthAutoExp = autoExp;
    m_synthAeTarget = aeTarget;
    m_synthTriggerMode = triggerMode;
}

void CameraWorker::triggerSyntheticSoftware()
{
    QMutexLocker locker(&m_synthMutex);
    ++m_synthPendingTriggers;
}

QImage CameraWorker::takeLatestPreviewFrame(qint64 &timestampMs)
{
    QMutexLocker locker(&m_previewMutex);
    m_previewPending = false;
    timestampMs = m_latestPreviewTimestampMs;
    if (!m_latestPreviewFrame.isNull()) {
        ++m_previewFramesEmitted;
    }
    return m_latestPreviewFrame;
}

void CameraWorker::getPreviewStats(qulonglong &received, qulonglong &emitted, qulonglong &dropped)
{
    QMutexLocker locker(&m_previewMutex);
    received = m_previewFramesReceived;
    emitted = m_previewFramesEmitted;
    dropped = m_previewFramesDropped;
}

QImage CameraWorker::renderSyntheticFrame(int frameIndex)
{
    double exposureMs = 15.0;
    int gain = 4;
    bool autoExp = true;
    int aeTarget = 120;
    {
        QMutexLocker locker(&m_synthMutex);
        exposureMs = m_synthExposureMs;
        gain = m_synthGain;
        autoExp = m_synthAutoExp;
        aeTarget = m_synthAeTarget;
    }

    QImage img(m_width, m_height, QImage::Format_RGB888);
    const int baseLevel = autoExp
        ? std::clamp(aeTarget, 30, 220)
        : std::clamp(static_cast<int>(exposureMs * 4.0 + gain * 3), 20, 240);
    img.fill(QColor(baseLevel, baseLevel, std::min(255, baseLevel + 10)));

    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Draw calibration ruler ticks across the middle band (10 px minor spacing = 1000 px/mm)
    const int bandTop = m_height / 2 - 30;
    const int bandBottom = m_height / 2 + 30;
    painter.setPen(QPen(QColor(30, 30, 30), 2));
    for (int x = 40; x < m_width - 40; x += 10) {
        const bool major = ((x - 40) % 50) == 0;
        painter.drawLine(x, bandTop + (major ? 0 : 12), x, bandBottom - (major ? 0 : 12));
    }

    // Draw a moving simulated specimen
    const double t = frameIndex * 0.05;
    const int cx = static_cast<int>(m_width * 0.5 + std::cos(t) * (m_width * 0.22));
    const int cy = static_cast<int>(m_height * 0.3 + std::sin(t * 1.3) * (m_height * 0.15));
    painter.setBrush(QColor(60, 120, 80));
    painter.setPen(QPen(QColor(20, 60, 30), 2));
    painter.drawEllipse(QPoint(cx, cy), 42, 22);
    painter.end();
    return img;
}

void CameraWorker::process()
{
    unsigned char *pRawBuffer = nullptr;
    CameraSdkStatus status;

    QElapsedTimer fpsTimer;
    fpsTimer.start();
    QElapsedTimer statsTimer;
    statsTimer.start();
    int frameCount = 0;
    int synthIndex = 0;

    while (!m_stopRequested) {
        if (m_synthetic) {
            bool canCapture = true;
            {
                QMutexLocker locker(&m_synthMutex);
                if (m_synthTriggerMode == 1) {
                    if (m_synthPendingTriggers > 0) {
                        --m_synthPendingTriggers;
                    } else {
                        canCapture = false;
                    }
                } else if (m_synthTriggerMode == 2) {
                    canCapture = false;
                }
            }

            if (!canCapture) {
                QThread::msleep(20);
                continue;
            }

            QImage frameCopy = renderSyntheticFrame(++synthIndex);
            const qint64 timestampMs = QDateTime::currentMSecsSinceEpoch();

            ++frameCount;
            if (fpsTimer.elapsed() >= 1000) {
                double fps = frameCount * 1000.0 / fpsTimer.elapsed();
                emit fpsChanged(fps);
                fpsTimer.restart();
                frameCount = 0;
            }

            emit frameReady(frameCopy, timestampMs);

            bool shouldNotifyPreview = false;
            {
                QMutexLocker locker(&m_previewMutex);
                ++m_previewFramesReceived;
                m_latestPreviewFrame = frameCopy;
                m_latestPreviewTimestampMs = timestampMs;
                if (!m_previewPending) {
                    m_previewPending = true;
                    shouldNotifyPreview = true;
                } else {
                    ++m_previewFramesDropped;
                }
            }
            if (shouldNotifyPreview) {
                emit previewFrameAvailable();
            }

            if (statsTimer.elapsed() >= 1000) {
                qulonglong queueSize = 0;
                qulonglong droppedFrames = 0;
                {
                    QMutexLocker locker(&m_previewMutex);
                    queueSize = m_previewPending ? 1ULL : 0ULL;
                    droppedFrames = m_previewFramesDropped;
                }
                emit queueStatsChanged(queueSize, droppedFrames);
                statsTimer.restart();
            }

            QThread::msleep(33);
            continue;
        }

        // 1. Get raw image buffer with a timeout (1000ms)
        status = CameraGetImageBuffer(m_hCamera, &m_frameHead, &pRawBuffer, 1000);

        if (status == CAMERA_STATUS_SUCCESS) {
            frameCount++;
            if (fpsTimer.elapsed() >= 1000) {
                double fps = frameCount * 1000.0 / fpsTimer.elapsed();
                emit fpsChanged(fps);

                fpsTimer.restart();
                frameCount = 0;
            }

            // 2. Process raw data to RGB
            status = CameraImageProcess(m_hCamera, pRawBuffer, m_pRgbBuffer, &m_frameHead);

            if (status == CAMERA_STATUS_SUCCESS) {
                qint64 timestampMs = QDateTime::currentMSecsSinceEpoch();

                // 3. Create QImage from the processed RGB buffer
                QImage img(m_pRgbBuffer, m_frameHead.iWidth, m_frameHead.iHeight, QImage::Format_RGB888);
                QImage frameCopy = img.copy();

                // Emit to direct consumers such as the recorder from the capture thread.
                emit frameReady(frameCopy, timestampMs);

                // Coalesce preview updates so the main-thread event queue cannot accumulate frames.
                bool shouldNotifyPreview = false;
                {
                    QMutexLocker locker(&m_previewMutex);
                    ++m_previewFramesReceived;
                    m_latestPreviewFrame = frameCopy;
                    m_latestPreviewTimestampMs = timestampMs;
                    if (!m_previewPending) {
                        m_previewPending = true;
                        shouldNotifyPreview = true;
                    } else {
                        ++m_previewFramesDropped;
                    }
                }
                if (shouldNotifyPreview) {
                    emit previewFrameAvailable();
                }
            }

            // 5. Release the raw buffer back to the SDK so it can be reused for new frames
            CameraReleaseImageBuffer(m_hCamera, pRawBuffer);

            if (statsTimer.elapsed() >= 1000) {
                qulonglong queueSize = 0;
                qulonglong droppedFrames = 0;
                {
                    QMutexLocker locker(&m_previewMutex);
                    queueSize = m_previewPending ? 1ULL : 0ULL;
                    droppedFrames = m_previewFramesDropped;
                }
                emit queueStatsChanged(queueSize, droppedFrames);
                statsTimer.restart();
            }
        }
    }

    emit finished();
}

// =============================================================================
// RemoteCameraClientWorker Declaration & Definition
// =============================================================================

struct RemoteCameraCachedState {
    bool connected = false;
    bool isOpen = false;
    bool isRunning = false;
    bool autoExposure = true;
    double exposureTimeMs = 15.0;
    int analogGain = 1;
    int aeTarget = 120;
    double exposureMinMs = 0.001;
    double exposureMaxMs = 1000.0;
    double exposureStepMs = 0.001;
    int gainMin = 1;
    int gainMax = 400;
    bool remoteRecording = false;
};

struct PendingRpcResult {
    bool completed = false;
    bool ok = false;
    QJsonObject result;
    QString error;
};

class RemoteCameraClientWorker : public QObject
{
    Q_OBJECT
public:
    explicit RemoteCameraClientWorker(const QString &host, quint16 port, int jpegQuality);
    ~RemoteCameraClientWorker() override;

    QImage takeLatestPreviewFrame(qint64 &timestampMs);
    void getPreviewStats(qulonglong &received, qulonglong &emitted, qulonglong &dropped);
    RemoteCameraCachedState cachedState() const;
    void setEndpoint(const QString &host, quint16 port);
    void setJpegQuality(int quality);

    bool sendCommandSync(const QString &cmd, const QJsonObject &params,
                         QJsonObject *resultOut = nullptr,
                         QString *errorOut = nullptr,
                         int timeoutMs = 2500);
    void disconnectFromServer();

public slots:
    void initializeSocket();
    void shutdownSocket();
    void connectAndSendPacket(quint64 reqId, QByteArray packet);

signals:
    void frameReady(QImage image, qint64 timestampMs);
    void previewFrameAvailable();
    void queueStatsChanged(qulonglong queueSize, qulonglong droppedFrames);
    void fpsChanged(double fps);
    void errorOccurred(QString message);

private slots:
    void onReadyRead();
    void onSocketDisconnected();
    void onSocketError(QAbstractSocket::SocketError socketError);

private:
    void handleJsonPayload(const QByteArray &payload);
    void handleFramePayload(const QByteArray &payload);
    void updateCachedStateFromJson(const QJsonObject &obj);
    void failAllPendingRpcs(const QString &reason);

    mutable QMutex m_stateMutex;
    RemoteCameraCachedState m_state;
    QString m_host;
    quint16 m_port;
    int m_jpegQuality;

    QMutex m_rpcMutex;
    QWaitCondition m_rpcWait;
    quint64 m_nextReqId = 0;
    QHash<quint64, PendingRpcResult> m_pendingRpcs;

    QTcpSocket *m_socket = nullptr;
    QByteArray m_readBuffer;
    QElapsedTimer m_fpsTimer;
    QElapsedTimer m_statsTimer;
    int m_fpsFrameCount = 0;

    QMutex m_previewMutex;
    QImage m_latestPreviewFrame;
    qint64 m_latestPreviewTimestampMs = 0;
    qulonglong m_previewFramesReceived = 0;
    qulonglong m_previewFramesEmitted = 0;
    qulonglong m_previewFramesDropped = 0;
    bool m_previewPending = false;
};

RemoteCameraClientWorker::RemoteCameraClientWorker(const QString &host, quint16 port, int jpegQuality)
    : m_host(host), m_port(port), m_jpegQuality(jpegQuality)
{
}

RemoteCameraClientWorker::~RemoteCameraClientWorker()
{
    shutdownSocket();
}

void RemoteCameraClientWorker::initializeSocket()
{
    if (m_socket) {
        return;
    }
    m_socket = new QTcpSocket(this);
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    connect(m_socket, &QTcpSocket::readyRead, this, &RemoteCameraClientWorker::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &RemoteCameraClientWorker::onSocketDisconnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &RemoteCameraClientWorker::onSocketError);
    m_fpsTimer.start();
    m_statsTimer.start();
}

void RemoteCameraClientWorker::shutdownSocket()
{
    if (m_socket) {
        m_socket->disconnect(this);
        if (m_socket->state() != QAbstractSocket::UnconnectedState) {
            m_socket->disconnectFromHost();
        }
        delete m_socket;
        m_socket = nullptr;
    }
    m_readBuffer.clear();
    {
        QMutexLocker locker(&m_stateMutex);
        m_state.connected = false;
        m_state.isOpen = false;
        m_state.isRunning = false;
        m_state.remoteRecording = false;
    }
    failAllPendingRpcs("Remote camera connection closed.");
}

void RemoteCameraClientWorker::setEndpoint(const QString &host, quint16 port)
{
    QMutexLocker locker(&m_stateMutex);
    m_host = host;
    m_port = port;
}

void RemoteCameraClientWorker::setJpegQuality(int quality)
{
    QMutexLocker locker(&m_stateMutex);
    m_jpegQuality = std::clamp(quality, 0, 100);
}

RemoteCameraCachedState RemoteCameraClientWorker::cachedState() const
{
    QMutexLocker locker(&m_stateMutex);
    return m_state;
}

QImage RemoteCameraClientWorker::takeLatestPreviewFrame(qint64 &timestampMs)
{
    QMutexLocker locker(&m_previewMutex);
    m_previewPending = false;
    timestampMs = m_latestPreviewTimestampMs;
    if (!m_latestPreviewFrame.isNull()) {
        ++m_previewFramesEmitted;
    }
    return m_latestPreviewFrame;
}

void RemoteCameraClientWorker::getPreviewStats(qulonglong &received, qulonglong &emitted, qulonglong &dropped)
{
    QMutexLocker locker(&m_previewMutex);
    received = m_previewFramesReceived;
    emitted = m_previewFramesEmitted;
    dropped = m_previewFramesDropped;
}

void RemoteCameraClientWorker::disconnectFromServer()
{
    QMetaObject::invokeMethod(this, &RemoteCameraClientWorker::shutdownSocket, Qt::BlockingQueuedConnection);
}

void RemoteCameraClientWorker::connectAndSendPacket(quint64 reqId, QByteArray packet)
{
    if (!m_socket) {
        initializeSocket();
    }

    QString host;
    quint16 port = CameraNet::kDefaultServerPort;
    {
        QMutexLocker locker(&m_stateMutex);
        host = m_host;
        port = m_port;
    }

    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        m_socket->abort();
        m_readBuffer.clear();
        m_socket->connectToHost(host, port);
        if (!m_socket->waitForConnected(2000)) {
            const QString err = QString("Could not connect to remote camera server at %1:%2 (%3)")
                                    .arg(host)
                                    .arg(port)
                                    .arg(m_socket->errorString());
            {
                QMutexLocker rpcLock(&m_rpcMutex);
                if (m_pendingRpcs.contains(reqId)) {
                    auto &entry = m_pendingRpcs[reqId];
                    entry.completed = true;
                    entry.ok = false;
                    entry.error = err;
                    m_rpcWait.wakeAll();
                }
            }
            emit errorOccurred(err);
            return;
        }
        m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        {
            QMutexLocker locker(&m_stateMutex);
            m_state.connected = true;
        }
    }

    const qint64 written = m_socket->write(packet);
    m_socket->flush();
    if (written != packet.size()) {
        const QString err = QString("Failed to write command to remote camera server: %1")
                                .arg(m_socket->errorString());
        QMutexLocker rpcLock(&m_rpcMutex);
        if (m_pendingRpcs.contains(reqId)) {
            auto &entry = m_pendingRpcs[reqId];
            entry.completed = true;
            entry.ok = false;
            entry.error = err;
            m_rpcWait.wakeAll();
        }
    }
}

bool RemoteCameraClientWorker::sendCommandSync(const QString &cmd, const QJsonObject &params,
                                               QJsonObject *resultOut, QString *errorOut,
                                               int timeoutMs)
{
    quint64 reqId = 0;
    {
        QMutexLocker rpcLock(&m_rpcMutex);
        reqId = ++m_nextReqId;
        m_pendingRpcs.insert(reqId, PendingRpcResult{});
    }

    QJsonObject reqObj;
    reqObj["id"] = static_cast<qint64>(reqId);
    reqObj["cmd"] = cmd;
    reqObj["params"] = params;
    const QByteArray packet = CameraNet::packJsonPacket(reqObj);

    if (QThread::currentThread() == this->thread()) {
        connectAndSendPacket(reqId, packet);
    } else {
        QMetaObject::invokeMethod(this, [this, reqId, packet]() {
            connectAndSendPacket(reqId, packet);
        }, Qt::QueuedConnection);
    }

    QMutexLocker rpcLock(&m_rpcMutex);
    QElapsedTimer timer;
    timer.start();
    while (m_pendingRpcs.contains(reqId) && !m_pendingRpcs[reqId].completed) {
        const qint64 remaining = timeoutMs - timer.elapsed();
        if (remaining <= 0) {
            m_pendingRpcs.remove(reqId);
            if (errorOut) {
                *errorOut = QString("Remote camera command '%1' timed out.").arg(cmd);
            }
            return false;
        }
        m_rpcWait.wait(&m_rpcMutex, static_cast<unsigned long>(remaining));
    }

    const PendingRpcResult res = m_pendingRpcs.take(reqId);
    if (resultOut) {
        *resultOut = res.result;
    }
    if (errorOut) {
        *errorOut = res.error;
    }
    return res.ok;
}

void RemoteCameraClientWorker::onSocketDisconnected()
{
    {
        QMutexLocker locker(&m_stateMutex);
        m_state.connected = false;
        m_state.isRunning = false;
        m_state.remoteRecording = false;
    }
    failAllPendingRpcs("Disconnected from remote camera server.");
}

void RemoteCameraClientWorker::onSocketError(QAbstractSocket::SocketError /*socketError*/)
{
    if (!m_socket) {
        return;
    }
    const QString msg = m_socket->errorString();
    failAllPendingRpcs(msg);
}

void RemoteCameraClientWorker::failAllPendingRpcs(const QString &reason)
{
    QMutexLocker rpcLock(&m_rpcMutex);
    for (auto it = m_pendingRpcs.begin(); it != m_pendingRpcs.end(); ++it) {
        if (!it.value().completed) {
            it.value().completed = true;
            it.value().ok = false;
            it.value().error = reason;
        }
    }
    m_rpcWait.wakeAll();
}

void RemoteCameraClientWorker::onReadyRead()
{
    if (!m_socket) {
        return;
    }
    m_readBuffer.append(m_socket->readAll());

    while (m_readBuffer.size() >= CameraNet::kPacketHeaderSize) {
        const uchar *hdr = reinterpret_cast<const uchar *>(m_readBuffer.constData());
        const quint8 packetType = hdr[0];
        const quint32 payloadLen = qFromBigEndian<quint32>(hdr + 1);

        if (payloadLen > 128U * 1024U * 1024U) {
            emit errorOccurred("Invalid packet length from remote camera server; resetting connection.");
            m_socket->abort();
            m_readBuffer.clear();
            return;
        }

        const int totalPacketSize = CameraNet::kPacketHeaderSize + static_cast<int>(payloadLen);
        if (m_readBuffer.size() < totalPacketSize) {
            break;
        }

        const QByteArray payload = m_readBuffer.mid(CameraNet::kPacketHeaderSize, static_cast<int>(payloadLen));
        m_readBuffer.remove(0, totalPacketSize);

        if (packetType == CameraNet::kPacketTypeJson) {
            handleJsonPayload(payload);
        } else if (packetType == CameraNet::kPacketTypeFrame) {
            handleFramePayload(payload);
        }
    }
}

void RemoteCameraClientWorker::updateCachedStateFromJson(const QJsonObject &obj)
{
    QMutexLocker locker(&m_stateMutex);
    if (obj.contains("is_open")) m_state.isOpen = obj.value("is_open").toBool();
    if (obj.contains("is_running")) m_state.isRunning = obj.value("is_running").toBool();
    if (obj.contains("auto_exposure")) m_state.autoExposure = obj.value("auto_exposure").toBool();
    if (obj.contains("exposure_time_ms")) m_state.exposureTimeMs = obj.value("exposure_time_ms").toDouble();
    if (obj.contains("analog_gain")) m_state.analogGain = obj.value("analog_gain").toInt();
    if (obj.contains("ae_target")) m_state.aeTarget = obj.value("ae_target").toInt();
    if (obj.contains("exposure_min_ms")) m_state.exposureMinMs = obj.value("exposure_min_ms").toDouble();
    if (obj.contains("exposure_max_ms")) m_state.exposureMaxMs = obj.value("exposure_max_ms").toDouble();
    if (obj.contains("exposure_step_ms")) m_state.exposureStepMs = obj.value("exposure_step_ms").toDouble();
    if (obj.contains("gain_min")) m_state.gainMin = obj.value("gain_min").toInt();
    if (obj.contains("gain_max")) m_state.gainMax = obj.value("gain_max").toInt();
    if (obj.contains("remote_recording")) m_state.remoteRecording = obj.value("remote_recording").toBool();
}

void RemoteCameraClientWorker::handleJsonPayload(const QByteArray &payload)
{
    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject()) {
        return;
    }
    const QJsonObject obj = doc.object();

    if (obj.contains("id")) {
        const quint64 reqId = static_cast<quint64>(obj.value("id").toInteger());
        const bool ok = obj.value("ok").toBool(false);
        const QJsonObject resultObj = obj.value("result").toObject();
        const QString errorStr = obj.value("error").toString();

        updateCachedStateFromJson(resultObj);

        QMutexLocker rpcLock(&m_rpcMutex);
        if (m_pendingRpcs.contains(reqId)) {
            auto &entry = m_pendingRpcs[reqId];
            entry.completed = true;
            entry.ok = ok;
            entry.result = resultObj;
            entry.error = errorStr;
            m_rpcWait.wakeAll();
        }
        return;
    }

    const QString eventType = obj.value("event").toString();
    if (eventType == "fps") {
        emit fpsChanged(obj.value("fps").toDouble());
    } else if (eventType == "stats") {
        updateCachedStateFromJson(obj);
        const qulonglong qSize = static_cast<qulonglong>(obj.value("queue_size").toInteger());
        const qulonglong dropped = static_cast<qulonglong>(obj.value("dropped_frames").toInteger());
        emit queueStatsChanged(qSize, dropped);
    } else if (eventType == "error") {
        emit errorOccurred(obj.value("message").toString("Remote camera error"));
    }
}

void RemoteCameraClientWorker::handleFramePayload(const QByteArray &payload)
{
    CameraNet::FramePacketHeader hdr;
    if (!CameraNet::unpackFrameHeader(reinterpret_cast<const uchar *>(payload.constData()),
                                      payload.size(), hdr)) {
        return;
    }

    {
        QMutexLocker locker(&m_stateMutex);
        m_state.exposureTimeMs = hdr.exposureTimeMs;
        m_state.analogGain = hdr.analogGain;
        m_state.autoExposure = hdr.autoExposure;
        m_state.aeTarget = hdr.aeTarget;
        m_state.remoteRecording = hdr.remoteRecording;
    }

    const uchar *imgData = reinterpret_cast<const uchar *>(payload.constData()) + CameraNet::kFrameHeaderSize;
    const int imgDataLen = payload.size() - CameraNet::kFrameHeaderSize;
    if (imgDataLen <= 0) {
        return;
    }

    QImage decoded;
    if (hdr.encoding == CameraNet::kFrameEncodingJpeg) {
        decoded = QImage::fromData(imgData, imgDataLen, "JPG");
        if (!decoded.isNull() && decoded.format() != QImage::Format_RGB888) {
            decoded = decoded.convertToFormat(QImage::Format_RGB888);
        }
    } else if (hdr.encoding == CameraNet::kFrameEncodingRawRgb) {
        const int expectedBytes = static_cast<int>(hdr.width) * static_cast<int>(hdr.height) * 3;
        if (imgDataLen >= expectedBytes && hdr.width > 0 && hdr.height > 0) {
            QImage rawImg(imgData, static_cast<int>(hdr.width), static_cast<int>(hdr.height),
                          static_cast<int>(hdr.width) * 3, QImage::Format_RGB888);
            decoded = rawImg.copy();
        }
    }

    if (decoded.isNull()) {
        return;
    }

    ++m_fpsFrameCount;
    if (m_fpsTimer.elapsed() >= 1000) {
        const double fps = m_fpsFrameCount * 1000.0 / m_fpsTimer.elapsed();
        emit fpsChanged(fps);
        m_fpsTimer.restart();
        m_fpsFrameCount = 0;
    }

    const qint64 timestampMs = hdr.timestampMs > 0 ? hdr.timestampMs : QDateTime::currentMSecsSinceEpoch();
    emit frameReady(decoded, timestampMs);

    bool shouldNotifyPreview = false;
    {
        QMutexLocker locker(&m_previewMutex);
        ++m_previewFramesReceived;
        m_latestPreviewFrame = decoded;
        m_latestPreviewTimestampMs = timestampMs;
        if (!m_previewPending) {
            m_previewPending = true;
            shouldNotifyPreview = true;
        } else {
            ++m_previewFramesDropped;
        }
    }
    if (shouldNotifyPreview) {
        emit previewFrameAvailable();
    }

    if (m_statsTimer.elapsed() >= 1000) {
        qulonglong queueSize = 0;
        qulonglong droppedFrames = 0;
        {
            QMutexLocker locker(&m_previewMutex);
            queueSize = m_previewPending ? 1ULL : 0ULL;
            droppedFrames = m_previewFramesDropped;
        }
        emit queueStatsChanged(queueSize, droppedFrames);
        m_statsTimer.restart();
    }
}

// =============================================================================
// MindVisionCameraPrivate
// =============================================================================

class MindVisionCameraPrivate
{
public:
    MindVisionCameraPrivate()
        : m_hCamera(0),
          m_isOpen(false),
          m_remoteMode(false),
          m_remoteHost("127.0.0.1"),
          m_remotePort(CameraNet::kDefaultServerPort),
          m_remoteJpegQuality(85),
          m_syntheticMode(false),
          m_synthRoi(false),
          m_synthAutoExposure(true),
          m_synthExposureTimeMs(15.0),
          m_synthAnalogGain(4),
          m_synthAeTarget(120),
          m_synthTriggerMode(0),
          m_workerThread(nullptr),
          m_worker(nullptr),
          m_remoteThread(nullptr),
          m_remoteWorker(nullptr),
          m_recordingTarget(nullptr)
    {}

    CameraHandle m_hCamera;
    tSdkCameraDevInfo m_devInfo;
    tSdkCameraCapbility m_capInfo;
    bool m_isOpen;

    // Remote client configuration
    bool m_remoteMode;
    QString m_remoteHost;
    quint16 m_remotePort;
    int m_remoteJpegQuality;

    // Synthetic mode configuration
    bool m_syntheticMode;
    bool m_synthRoi;
    bool m_synthAutoExposure;
    double m_synthExposureTimeMs;
    int m_synthAnalogGain;
    int m_synthAeTarget;
    int m_synthTriggerMode;

    QThread *m_workerThread;
    CameraWorker *m_worker;

    QThread *m_remoteThread;
    RemoteCameraClientWorker *m_remoteWorker;

    VideoThread *m_recordingTarget;
    QMetaObject::Connection m_recordingConnection;

    void ensureRemoteWorker(MindVisionCamera *owner)
    {
        if (m_remoteWorker && m_remoteThread) {
            return;
        }
        m_remoteThread = new QThread;
        m_remoteWorker = new RemoteCameraClientWorker(m_remoteHost, m_remotePort, m_remoteJpegQuality);
        m_remoteWorker->moveToThread(m_remoteThread);

        QObject::connect(m_remoteThread, &QThread::started,
                         m_remoteWorker, &RemoteCameraClientWorker::initializeSocket);
        QObject::connect(m_remoteWorker, &RemoteCameraClientWorker::previewFrameAvailable,
                         owner, &MindVisionCamera::deliverLatestFrame);
        QObject::connect(m_remoteWorker, &RemoteCameraClientWorker::queueStatsChanged,
                         owner, &MindVisionCamera::queueStatsChanged);
        QObject::connect(m_remoteWorker, &RemoteCameraClientWorker::fpsChanged,
                         owner, &MindVisionCamera::fpsChanged);
        QObject::connect(m_remoteWorker, &RemoteCameraClientWorker::errorOccurred,
                         owner, &MindVisionCamera::errorOccurred);

        m_remoteThread->start();
    }

    void teardownRemoteWorker()
    {
        if (m_recordingConnection) {
            QObject::disconnect(m_recordingConnection);
            m_recordingConnection = QMetaObject::Connection();
        }
        if (m_remoteWorker) {
            m_remoteWorker->disconnectFromServer();
            if (m_remoteThread) {
                m_remoteThread->quit();
                m_remoteThread->wait();
                delete m_remoteThread;
                m_remoteThread = nullptr;
            }
            delete m_remoteWorker;
            m_remoteWorker = nullptr;
        }
    }
};

// =============================================================================
// MindVisionCamera Implementation
// =============================================================================

MindVisionCamera::MindVisionCamera(QObject *parent)
    : QObject(parent), d(new MindVisionCameraPrivate)
{
    const QString envHost = qEnvironmentVariable("MICROSCOPE_CAMERA_HOST",
                                                 qEnvironmentVariable("MINDVISION_REMOTE_HOST"));
    const QString envPort = qEnvironmentVariable("MICROSCOPE_CAMERA_PORT",
                                                 qEnvironmentVariable("MINDVISION_REMOTE_PORT"));
    const QString envMode = qEnvironmentVariable("MICROSCOPE_CAMERA_MODE").trimmed().toLower();
    const QString envQuality = qEnvironmentVariable("MICROSCOPE_CAMERA_JPEG_QUALITY");
    const QString envSynth = qEnvironmentVariable("MICROSCOPE_CAMERA_SYNTHETIC").trimmed().toLower();

    if (!envHost.isEmpty()) {
        d->m_remoteHost = envHost.trimmed();
        d->m_remoteMode = true;
    }
    if (!envPort.isEmpty()) {
        bool ok = false;
        const int port = envPort.toInt(&ok);
        if (ok && port >= 1 && port <= 65535) {
            d->m_remotePort = static_cast<quint16>(port);
        }
    }
    if (envMode == "remote") {
        d->m_remoteMode = true;
    } else if (envMode == "local") {
        d->m_remoteMode = false;
    }
    if (!envQuality.isEmpty()) {
        bool ok = false;
        const int q = envQuality.toInt(&ok);
        if (ok) {
            d->m_remoteJpegQuality = std::clamp(q, 0, 100);
        }
    }
    if (envSynth == "1" || envSynth == "true" || envSynth == "yes") {
        d->m_syntheticMode = true;
    }
}

MindVisionCamera::~MindVisionCamera()
{
    stop();
    close();
    d->teardownRemoteWorker();
    delete d;
}

void MindVisionCamera::setRemoteMode(bool enabled)
{
    if (d->m_remoteMode == enabled) {
        return;
    }
    if (isOpen()) {
        stop();
        close();
    }
    d->m_remoteMode = enabled;
}

bool MindVisionCamera::isRemoteMode() const
{
    return d->m_remoteMode;
}

void MindVisionCamera::setRemoteEndpoint(const QString &host, quint16 port)
{
    const QString trimmed = host.trimmed().isEmpty() ? QStringLiteral("127.0.0.1") : host.trimmed();
    const bool changed = (d->m_remoteHost != trimmed || d->m_remotePort != port);
    d->m_remoteHost = trimmed;
    d->m_remotePort = port;
    if (d->m_remoteWorker) {
        d->m_remoteWorker->setEndpoint(d->m_remoteHost, d->m_remotePort);
        if (changed && d->m_remoteMode && isOpen()) {
            stop();
            close();
        }
    }
}

QString MindVisionCamera::remoteHost() const
{
    return d->m_remoteHost;
}

quint16 MindVisionCamera::remotePort() const
{
    return d->m_remotePort;
}

bool MindVisionCamera::setRemoteJpegQuality(int quality)
{
    d->m_remoteJpegQuality = std::clamp(quality, 0, 100);
    if (d->m_remoteWorker) {
        d->m_remoteWorker->setJpegQuality(d->m_remoteJpegQuality);
        if (d->m_remoteMode && isOpen()) {
            QJsonObject params;
            params["jpeg_quality"] = d->m_remoteJpegQuality;
            return d->m_remoteWorker->sendCommandSync("set_stream_config", params);
        }
    }
    return true;
}

int MindVisionCamera::remoteJpegQuality() const
{
    return d->m_remoteJpegQuality;
}

void MindVisionCamera::setSyntheticMode(bool enabled)
{
    d->m_syntheticMode = enabled;
}

bool MindVisionCamera::isSyntheticMode() const
{
    return d->m_syntheticMode;
}

bool MindVisionCamera::startRemoteRecording(int width, int height, double fps, const QString &filename)
{
    if (!d->m_remoteMode || !d->m_remoteWorker) {
        return false;
    }
    QJsonObject params;
    params["width"] = width;
    params["height"] = height;
    params["fps"] = fps;
    params["filename"] = filename;
    return d->m_remoteWorker->sendCommandSync("start_recording", params);
}

bool MindVisionCamera::stopRemoteRecording()
{
    if (!d->m_remoteMode || !d->m_remoteWorker) {
        return false;
    }
    return d->m_remoteWorker->sendCommandSync("stop_recording", QJsonObject{});
}

bool MindVisionCamera::isRemoteRecording() const
{
    if (!d->m_remoteMode || !d->m_remoteWorker) {
        return false;
    }
    return d->m_remoteWorker->cachedState().remoteRecording;
}

bool MindVisionCamera::isOpen() const
{
    if (d->m_remoteMode) {
        return d->m_remoteWorker && d->m_remoteWorker->cachedState().isOpen;
    }
    return d->m_isOpen;
}

bool MindVisionCamera::isRunning() const
{
    if (d->m_remoteMode) {
        return d->m_remoteWorker && d->m_remoteWorker->cachedState().isRunning;
    }
    return d->m_workerThread != nullptr;
}

bool MindVisionCamera::open()
{
    if (d->m_remoteMode) {
        d->ensureRemoteWorker(this);
        d->m_remoteWorker->setEndpoint(d->m_remoteHost, d->m_remotePort);
        d->m_remoteWorker->setJpegQuality(d->m_remoteJpegQuality);

        QJsonObject params;
        params["jpeg_quality"] = d->m_remoteJpegQuality;
        QJsonObject result;
        QString err;
        if (!d->m_remoteWorker->sendCommandSync("open", params, &result, &err, 4000)) {
            if (!err.isEmpty()) {
                emit errorOccurred(err);
            }
            return false;
        }
        return true;
    }

    if (d->m_isOpen) return true;

    if (d->m_syntheticMode) {
        d->m_isOpen = true;
        std::cout << "Opened synthetic test-pattern camera." << std::endl;
        return true;
    }

    ensureSdkInitialized();

    int iCameraCounts = 1;
    tSdkCameraDevInfo tCameraList[1];

    if (CameraEnumerateDevice(tCameraList, &iCameraCounts) != CAMERA_STATUS_SUCCESS || iCameraCounts == 0) {
        std::cout << "No MindVision camera found." << std::endl;
        emit errorOccurred("No MindVision camera found.");
        return false;
    }

    std::cout << "Found camera: " << tCameraList[0].acFriendlyName << " (" << tCameraList[0].acProductName << ")" << std::endl;

    int status = CameraInit(&tCameraList[0], -1, -1, &d->m_hCamera);
    if (status != CAMERA_STATUS_SUCCESS) {
        std::cout << "Failed to initialize camera. Error code: " << status << std::endl;
        emit errorOccurred("Failed to initialize camera.");
        return false;
    }

    d->m_devInfo = tCameraList[0];
    d->m_isOpen = true;

    CameraGetCapability(d->m_hCamera, &d->m_capInfo);

    UINT uTrigCapability = 0;
    if (CameraGetExtTrigCapability(d->m_hCamera, &uTrigCapability) == CAMERA_STATUS_SUCCESS) {
        bool supportsLevelTrigger = (uTrigCapability & EXT_TRIG_MASK_LEVEL_MODE);
        std::cout << "External Trigger Capability Mask: " << uTrigCapability << std::endl;
        std::cout << "Supports Level Trigger (Bulb Mode): " << (supportsLevelTrigger ? "YES" : "NO") << std::endl;
    } else {
        std::cout << "Failed to get External Trigger Capability." << std::endl;
    }

    CameraSetIspOutFormat(d->m_hCamera, CAMERA_MEDIA_TYPE_RGB8);
    setAutoExposure(true);

    return true;
}

void MindVisionCamera::close()
{
    if (d->m_remoteMode) {
        if (d->m_remoteWorker) {
            stop();
            if (d->m_remoteWorker->cachedState().connected || d->m_remoteWorker->cachedState().isOpen) {
                d->m_remoteWorker->sendCommandSync("close", QJsonObject{}, nullptr, nullptr, 1500);
            }
            d->m_remoteWorker->disconnectFromServer();
        }
        return;
    }

    if (d->m_isOpen) {
        stop();

        if (!d->m_syntheticMode && d->m_hCamera != 0) {
            CameraUnInit(d->m_hCamera);
        }

        d->m_hCamera = 0;
        d->m_isOpen = false;
    }
}

bool MindVisionCamera::start()
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker || !d->m_remoteWorker->cachedState().isOpen) {
            emit errorOccurred("Remote camera is not open.");
            return false;
        }

        if (d->m_recordingConnection) {
            QObject::disconnect(d->m_recordingConnection);
            d->m_recordingConnection = QMetaObject::Connection();
        }
        if (d->m_recordingTarget != nullptr) {
            d->m_recordingConnection = connect(
                d->m_remoteWorker,
                &RemoteCameraClientWorker::frameReady,
                d->m_recordingTarget,
                &VideoThread::addFrame,
                Qt::DirectConnection);
        }

        QJsonObject params;
        params["jpeg_quality"] = d->m_remoteJpegQuality;
        QString err;
        if (!d->m_remoteWorker->sendCommandSync("start", params, nullptr, &err, 3000)) {
            if (!err.isEmpty()) {
                emit errorOccurred(err);
            }
            return false;
        }
        return true;
    }

    if (!d->m_isOpen) {
        emit errorOccurred("Camera is not open.");
        return false;
    }

    if (d->m_workerThread != nullptr) {
        return true;
    }

    int width = d->m_synthRoi ? 640 : 1280;
    int height = d->m_synthRoi ? 480 : 1024;

    if (!d->m_syntheticMode) {
        if (CameraPlay(d->m_hCamera) != CAMERA_STATUS_SUCCESS) {
            emit errorOccurred("Failed to start camera play.");
            return false;
        }

        tSdkImageResolution tResolution;
        if (CameraGetImageResolution(d->m_hCamera, &tResolution) != CAMERA_STATUS_SUCCESS) {
            emit errorOccurred("Failed to get camera resolution.");
            return false;
        }
        width = tResolution.iWidth;
        height = tResolution.iHeight;
    }

    d->m_workerThread = new QThread;
    d->m_worker = new CameraWorker(d->m_hCamera, width, height, d->m_syntheticMode);
    if (d->m_syntheticMode) {
        d->m_worker->setSyntheticParams(d->m_synthExposureTimeMs, d->m_synthAnalogGain,
                                        d->m_synthAutoExposure, d->m_synthAeTarget,
                                        d->m_synthTriggerMode);
    }
    d->m_worker->moveToThread(d->m_workerThread);

    connect(d->m_workerThread, &QThread::started, d->m_worker, &CameraWorker::process);
    connect(d->m_worker, &CameraWorker::previewFrameAvailable, this, &MindVisionCamera::deliverLatestFrame);
    connect(d->m_worker, &CameraWorker::queueStatsChanged, this, &MindVisionCamera::queueStatsChanged);
    connect(d->m_worker, &CameraWorker::fpsChanged, this, &MindVisionCamera::fpsChanged);

    if (d->m_recordingTarget != nullptr) {
        d->m_recordingConnection = connect(
            d->m_worker,
            &CameraWorker::frameReady,
            d->m_recordingTarget,
            &VideoThread::addFrame,
            Qt::DirectConnection);
    }

    connect(d->m_worker, &CameraWorker::finished, d->m_workerThread, &QThread::quit);
    connect(d->m_worker, &CameraWorker::finished, d->m_worker, &CameraWorker::deleteLater);
    connect(d->m_workerThread, &QThread::finished, d->m_workerThread, &QThread::deleteLater);

    d->m_workerThread->start();
    return true;
}

void MindVisionCamera::stop()
{
    if (d->m_remoteMode) {
        if (d->m_recordingConnection) {
            QObject::disconnect(d->m_recordingConnection);
            d->m_recordingConnection = QMetaObject::Connection();
        }
        if (d->m_remoteWorker && d->m_remoteWorker->cachedState().isRunning) {
            d->m_remoteWorker->sendCommandSync("stop", QJsonObject{}, nullptr, nullptr, 1500);
        }
        return;
    }

    if (d->m_worker) {
        if (d->m_recordingConnection) {
            QObject::disconnect(d->m_recordingConnection);
            d->m_recordingConnection = QMetaObject::Connection();
        }
        d->m_worker->stop();
        if (d->m_workerThread) {
            d->m_workerThread->quit();
            d->m_workerThread->wait();
        }
        d->m_worker = nullptr;
        d->m_workerThread = nullptr;
    }

    if (d->m_isOpen && !d->m_syntheticMode && d->m_hCamera != 0) {
        CameraStop(d->m_hCamera);
    }
}

void MindVisionCamera::setRecordingTarget(VideoThread *target)
{
    if (d->m_recordingConnection) {
        QObject::disconnect(d->m_recordingConnection);
        d->m_recordingConnection = QMetaObject::Connection();
    }

    d->m_recordingTarget = target;
    if (d->m_remoteMode) {
        if (d->m_remoteWorker != nullptr && d->m_recordingTarget != nullptr) {
            d->m_recordingConnection = connect(
                d->m_remoteWorker,
                &RemoteCameraClientWorker::frameReady,
                d->m_recordingTarget,
                &VideoThread::addFrame,
                Qt::DirectConnection);
        }
    } else if (d->m_worker != nullptr && d->m_recordingTarget != nullptr) {
        d->m_recordingConnection = connect(
            d->m_worker,
            &CameraWorker::frameReady,
            d->m_recordingTarget,
            &VideoThread::addFrame,
            Qt::DirectConnection);
    }
}

void MindVisionCamera::clearRecordingTarget()
{
    if (d->m_recordingConnection) {
        QObject::disconnect(d->m_recordingConnection);
        d->m_recordingConnection = QMetaObject::Connection();
    }
    d->m_recordingTarget = nullptr;
}

void MindVisionCamera::deliverLatestFrame()
{
    if (d->m_remoteMode) {
        if (d->m_remoteWorker == nullptr) {
            return;
        }
        qint64 timestampMs = 0;
        QImage frame = d->m_remoteWorker->takeLatestPreviewFrame(timestampMs);
        if (!frame.isNull()) {
            emit frameReady(frame, timestampMs);
        }
        return;
    }

    if (d->m_worker == nullptr) {
        return;
    }

    qint64 timestampMs = 0;
    QImage frame = d->m_worker->takeLatestPreviewFrame(timestampMs);
    if (!frame.isNull()) {
        emit frameReady(frame, timestampMs);
    }
}

void MindVisionCamera::getFrameCallbackStats(qulonglong &received, qulonglong &emitted, qulonglong &dropped)
{
    if (d->m_remoteMode) {
        if (d->m_remoteWorker == nullptr) {
            received = 0;
            emitted = 0;
            dropped = 0;
            return;
        }
        d->m_remoteWorker->getPreviewStats(received, emitted, dropped);
        return;
    }

    if (d->m_worker == nullptr) {
        received = 0;
        emitted = 0;
        dropped = 0;
        return;
    }

    d->m_worker->getPreviewStats(received, emitted, dropped);
}

bool MindVisionCamera::setAutoExposure(bool enabled)
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return false;
        QJsonObject params{{"enabled", enabled}};
        return d->m_remoteWorker->sendCommandSync("set_auto_exposure", params);
    }
    if (!d->m_isOpen) return false;
    if (d->m_syntheticMode) {
        d->m_synthAutoExposure = enabled;
        if (d->m_worker) {
            d->m_worker->setSyntheticParams(d->m_synthExposureTimeMs, d->m_synthAnalogGain,
                                            d->m_synthAutoExposure, d->m_synthAeTarget,
                                            d->m_synthTriggerMode);
        }
        return true;
    }
    return CameraSetAeState(d->m_hCamera, enabled ? TRUE : FALSE) == CAMERA_STATUS_SUCCESS;
}

bool MindVisionCamera::setExposureTime(double exposureTimeMs)
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return false;
        QJsonObject params{{"exposure_time_ms", exposureTimeMs}};
        return d->m_remoteWorker->sendCommandSync("set_exposure_time", params);
    }
    if (!d->m_isOpen) return false;
    if (d->m_syntheticMode) {
        d->m_synthExposureTimeMs = std::clamp(exposureTimeMs, 0.001, 1000.0);
        if (d->m_worker) {
            d->m_worker->setSyntheticParams(d->m_synthExposureTimeMs, d->m_synthAnalogGain,
                                            d->m_synthAutoExposure, d->m_synthAeTarget,
                                            d->m_synthTriggerMode);
        }
        return true;
    }
    double exposureTimeUs = exposureTimeMs * 1000.0;
    return CameraSetExposureTime(d->m_hCamera, exposureTimeUs) == CAMERA_STATUS_SUCCESS;
}

bool MindVisionCamera::setAnalogGain(int gain)
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return false;
        QJsonObject params{{"gain", gain}};
        return d->m_remoteWorker->sendCommandSync("set_analog_gain", params);
    }
    if (!d->m_isOpen) return false;
    if (d->m_syntheticMode) {
        d->m_synthAnalogGain = std::clamp(gain, 1, 64);
        if (d->m_worker) {
            d->m_worker->setSyntheticParams(d->m_synthExposureTimeMs, d->m_synthAnalogGain,
                                            d->m_synthAutoExposure, d->m_synthAeTarget,
                                            d->m_synthTriggerMode);
        }
        return true;
    }
    return CameraSetAnalogGain(d->m_hCamera, gain) == CAMERA_STATUS_SUCCESS;
}

bool MindVisionCamera::setAeTarget(int target)
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return false;
        QJsonObject params{{"target", target}};
        return d->m_remoteWorker->sendCommandSync("set_ae_target", params);
    }
    if (!d->m_isOpen) return false;
    if (d->m_syntheticMode) {
        d->m_synthAeTarget = std::clamp(target, 0, 255);
        if (d->m_worker) {
            d->m_worker->setSyntheticParams(d->m_synthExposureTimeMs, d->m_synthAnalogGain,
                                            d->m_synthAutoExposure, d->m_synthAeTarget,
                                            d->m_synthTriggerMode);
        }
        return true;
    }
    return CameraSetAeTarget(d->m_hCamera, target) == CAMERA_STATUS_SUCCESS;
}

bool MindVisionCamera::getAutoExposure()
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return false;
        return d->m_remoteWorker->cachedState().autoExposure;
    }
    if (!d->m_isOpen) return false;
    if (d->m_syntheticMode) return d->m_synthAutoExposure;
    BOOL state = FALSE;
    CameraGetAeState(d->m_hCamera, &state);
    return state == TRUE;
}

double MindVisionCamera::getExposureTime()
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return 0.0;
        return d->m_remoteWorker->cachedState().exposureTimeMs;
    }
    if (!d->m_isOpen) return 0.0;
    if (d->m_syntheticMode) return d->m_synthExposureTimeMs;
    double timeUs = 0;
    CameraGetExposureTime(d->m_hCamera, &timeUs);
    return timeUs / 1000.0;
}

int MindVisionCamera::getAnalogGain()
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return 0;
        return d->m_remoteWorker->cachedState().analogGain;
    }
    if (!d->m_isOpen) return 0;
    if (d->m_syntheticMode) return d->m_synthAnalogGain;
    int gain = 0;
    CameraGetAnalogGain(d->m_hCamera, &gain);
    return gain;
}

int MindVisionCamera::getAeTarget()
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return 0;
        return d->m_remoteWorker->cachedState().aeTarget;
    }
    if (!d->m_isOpen) return 0;
    if (d->m_syntheticMode) return d->m_synthAeTarget;
    int target = 0;
    CameraGetAeTarget(d->m_hCamera, &target);
    return target;
}

void MindVisionCamera::getExposureTimeRange(double &minMs, double &maxMs)
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) {
            minMs = 0;
            maxMs = 0;
            return;
        }
        const auto st = d->m_remoteWorker->cachedState();
        minMs = st.exposureMinMs;
        maxMs = st.exposureMaxMs;
        return;
    }

    if (!d->m_isOpen) {
        minMs = 0;
        maxMs = 0;
        return;
    }

    if (d->m_syntheticMode) {
        minMs = 0.01;
        maxMs = 1000.0;
        return;
    }

    double minUs = 0, maxUs = 0, stepUs = 0;
    if (CameraGetExposureTimeRange(d->m_hCamera, &minUs, &maxUs, &stepUs) == CAMERA_STATUS_SUCCESS) {
        minMs = minUs / 1000.0;
        maxMs = maxUs / 1000.0;
        return;
    }

    double lineTime = 0;
    CameraGetExposureLineTime(d->m_hCamera, &lineTime);

    if (lineTime > 0) {
        minMs = d->m_capInfo.sExposeDesc.uiExposeTimeMin * lineTime / 1000.0;
        maxMs = d->m_capInfo.sExposeDesc.uiExposeTimeMax * lineTime / 1000.0;
    } else {
        minMs = 0.1;
        maxMs = 1000.0;
    }
}

double MindVisionCamera::getExposureTimeStep()
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return 0.0;
        return d->m_remoteWorker->cachedState().exposureStepMs;
    }
    if (!d->m_isOpen) return 0.0;
    if (d->m_syntheticMode) return 0.01;

    double minUs = 0, maxUs = 0, stepUs = 0;
    if (CameraGetExposureTimeRange(d->m_hCamera, &minUs, &maxUs, &stepUs) == CAMERA_STATUS_SUCCESS) {
        return stepUs / 1000.0;
    }

    double lineTime = 0;
    CameraGetExposureLineTime(d->m_hCamera, &lineTime);
    return lineTime / 1000.0;
}

void MindVisionCamera::getAnalogGainRange(int &min, int &max)
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) {
            min = 0;
            max = 0;
            return;
        }
        const auto st = d->m_remoteWorker->cachedState();
        min = st.gainMin;
        max = st.gainMax;
        return;
    }

    if (!d->m_isOpen) {
        min = 0;
        max = 0;
        return;
    }
    if (d->m_syntheticMode) {
        min = 1;
        max = 64;
        return;
    }
    min = d->m_capInfo.sExposeDesc.uiAnalogGainMin;
    max = d->m_capInfo.sExposeDesc.uiAnalogGainMax;
}

bool MindVisionCamera::setRoi(bool enable)
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return false;
        QJsonObject params{{"enable", enable}};
        return d->m_remoteWorker->sendCommandSync("set_roi", params);
    }
    if (!d->m_isOpen) return false;

    bool wasRunning = (d->m_workerThread != nullptr);
    if (wasRunning) {
        stop();
    }

    if (d->m_syntheticMode) {
        d->m_synthRoi = enable;
        if (wasRunning) {
            return start();
        }
        return true;
    }

    tSdkImageResolution tResolution;
    CameraGetImageResolution(d->m_hCamera, &tResolution);

    if (enable) {
        tResolution.iIndex = 0xFF;
        tResolution.iWidth = 640;
        tResolution.iHeight = 480;

        int maxWidth = d->m_capInfo.pImageSizeDesc[0].iWidth;
        int maxHeight = d->m_capInfo.pImageSizeDesc[0].iHeight;

        if (maxWidth > 640 && maxHeight > 480) {
            tResolution.iHOffsetFOV = (maxWidth - 640) / 2;
            tResolution.iVOffsetFOV = (maxHeight - 480) / 2;
        } else {
            tResolution.iHOffsetFOV = 0;
            tResolution.iVOffsetFOV = 0;
        }
    } else {
        tResolution.iIndex = 0;
        tResolution.iWidth = d->m_capInfo.pImageSizeDesc[0].iWidth;
        tResolution.iHeight = d->m_capInfo.pImageSizeDesc[0].iHeight;
        tResolution.iHOffsetFOV = 0;
        tResolution.iVOffsetFOV = 0;
    }

    if (CameraSetImageResolution(d->m_hCamera, &tResolution) != CAMERA_STATUS_SUCCESS) {
        if (wasRunning) start();
        return false;
    }

    if (wasRunning) {
        return start();
    }
    return true;
}

bool MindVisionCamera::setTriggerMode(int mode)
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return false;
        QJsonObject params{{"mode", mode}};
        return d->m_remoteWorker->sendCommandSync("set_trigger_mode", params);
    }
    if (!d->m_isOpen) return false;
    if (d->m_syntheticMode) {
        d->m_synthTriggerMode = mode;
        if (d->m_worker) {
            d->m_worker->setSyntheticParams(d->m_synthExposureTimeMs, d->m_synthAnalogGain,
                                            d->m_synthAutoExposure, d->m_synthAeTarget,
                                            d->m_synthTriggerMode);
        }
        return true;
    }

    CameraSetTriggerCount(d->m_hCamera, 1);
    return CameraSetTriggerMode(d->m_hCamera, mode) == CAMERA_STATUS_SUCCESS;
}

bool MindVisionCamera::setTriggerCount(int count)
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return false;
        return d->m_remoteWorker->sendCommandSync("set_trigger_count", QJsonObject{{"count", count}});
    }
    if (!d->m_isOpen) return false;
    if (d->m_syntheticMode) return true;
    return CameraSetTriggerCount(d->m_hCamera, count) == CAMERA_STATUS_SUCCESS;
}

bool MindVisionCamera::setTriggerDelay(int delay_us)
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return false;
        return d->m_remoteWorker->sendCommandSync("set_trigger_delay", QJsonObject{{"delay_us", delay_us}});
    }
    if (!d->m_isOpen) return false;
    if (d->m_syntheticMode) return true;
    return CameraSetTriggerDelayTime(d->m_hCamera, delay_us) == CAMERA_STATUS_SUCCESS;
}

bool MindVisionCamera::setTriggerInterval(int interval_us)
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return false;
        return d->m_remoteWorker->sendCommandSync("set_trigger_interval", QJsonObject{{"interval_us", interval_us}});
    }
    if (!d->m_isOpen) return false;
    if (d->m_syntheticMode) return true;
    return CameraSetExtTrigIntervalTime(d->m_hCamera, interval_us) == CAMERA_STATUS_SUCCESS;
}

bool MindVisionCamera::setExternalTriggerSignalType(int type)
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return false;
        return d->m_remoteWorker->sendCommandSync("set_ext_trig_signal_type", QJsonObject{{"type", type}});
    }
    if (!d->m_isOpen) return false;
    if (d->m_syntheticMode) return true;
    return CameraSetExtTrigSignalType(d->m_hCamera, type) == CAMERA_STATUS_SUCCESS;
}

bool MindVisionCamera::setExternalTriggerJitterTime(int time_us)
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return false;
        return d->m_remoteWorker->sendCommandSync("set_ext_trig_jitter_time", QJsonObject{{"time_us", time_us}});
    }
    if (!d->m_isOpen) return false;
    if (d->m_syntheticMode) return true;
    return CameraSetExtTrigJitterTime(d->m_hCamera, time_us) == CAMERA_STATUS_SUCCESS;
}

bool MindVisionCamera::setExternalTriggerShutterMode(int mode)
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return false;
        return d->m_remoteWorker->sendCommandSync("set_ext_trig_shutter_mode", QJsonObject{{"mode", mode}});
    }
    if (!d->m_isOpen) return false;
    if (d->m_syntheticMode) return true;
    return CameraSetExtTrigShutterType(d->m_hCamera, mode) == CAMERA_STATUS_SUCCESS;
}

bool MindVisionCamera::setStrobeMode(int mode)
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return false;
        return d->m_remoteWorker->sendCommandSync("set_strobe_mode", QJsonObject{{"mode", mode}});
    }
    if (!d->m_isOpen) return false;
    if (d->m_syntheticMode) return true;
    return CameraSetStrobeMode(d->m_hCamera, mode) == CAMERA_STATUS_SUCCESS;
}

bool MindVisionCamera::setStrobePolarity(int polarity)
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return false;
        return d->m_remoteWorker->sendCommandSync("set_strobe_polarity", QJsonObject{{"polarity", polarity}});
    }
    if (!d->m_isOpen) return false;
    if (d->m_syntheticMode) return true;
    return CameraSetStrobePolarity(d->m_hCamera, polarity) == CAMERA_STATUS_SUCCESS;
}

bool MindVisionCamera::setStrobeDelayTime(int delay_us)
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return false;
        return d->m_remoteWorker->sendCommandSync("set_strobe_delay_time", QJsonObject{{"delay_us", delay_us}});
    }
    if (!d->m_isOpen) return false;
    if (d->m_syntheticMode) return true;
    return CameraSetStrobeDelayTime(d->m_hCamera, delay_us) == CAMERA_STATUS_SUCCESS;
}

bool MindVisionCamera::setStrobePulseWidth(int width_us)
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return false;
        return d->m_remoteWorker->sendCommandSync("set_strobe_pulse_width", QJsonObject{{"width_us", width_us}});
    }
    if (!d->m_isOpen) return false;
    if (d->m_syntheticMode) return true;
    return CameraSetStrobePulseWidth(d->m_hCamera, width_us) == CAMERA_STATUS_SUCCESS;
}

bool MindVisionCamera::triggerSoftware()
{
    if (d->m_remoteMode) {
        if (!d->m_remoteWorker) return false;
        return d->m_remoteWorker->sendCommandSync("trigger_software", QJsonObject{});
    }
    if (!d->m_isOpen) return false;
    if (d->m_syntheticMode) {
        if (d->m_worker) {
            d->m_worker->triggerSyntheticSoftware();
        }
        return true;
    }
    return CameraSoftTrigger(d->m_hCamera) == CAMERA_STATUS_SUCCESS;
}

#include "MindVisionCamera.moc"
