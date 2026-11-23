#include "SerialWorker.h"
#include "CameraNetworkProtocol.h"

#include <QAbstractSocket>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSerialPortInfo>

SerialWorker::SerialWorker(QObject *parent)
    : QObject(parent)
{
    connect(&m_serial, &QSerialPort::readyRead, this, &SerialWorker::handleReadyRead);
    connect(&m_serial, &QSerialPort::errorOccurred, this, &SerialWorker::handleError);
    connect(&m_socket, &QTcpSocket::readyRead, this, &SerialWorker::handleSocketReadyRead);
    connect(&m_socket, &QTcpSocket::disconnected, this, &SerialWorker::handleSocketDisconnected);
}

SerialWorker::~SerialWorker()
{
    disconnect_serial();
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.disconnect(this);
        m_socket.abort();
    }
}

void SerialWorker::register_log_callback(std::function<void(std::string)> cb)
{
    m_log_cb = std::move(cb);
}

void SerialWorker::register_status_callback(std::function<void(bool)> cb)
{
    m_status_cb = std::move(cb);
}

void SerialWorker::emitLog(const QString& msg)
{
    if (m_log_cb) {
        m_log_cb(msg.toStdString());
    }
    emit log_signal(msg);
}

void SerialWorker::emitStatus(bool connected)
{
    if (m_status_cb) {
        m_status_cb(connected);
    }
    emit connection_status(connected);
}

void SerialWorker::setRemoteMode(bool enabled)
{
    if (m_remoteMode == enabled) {
        return;
    }
    if (is_connected()) {
        disconnect_serial();
    }
    if (!enabled && m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.abort();
        m_socketReadBuffer.clear();
    }
    m_remoteMode = enabled;
}

bool SerialWorker::isRemoteMode() const
{
    return m_remoteMode;
}

void SerialWorker::setRemoteEndpoint(const std::string& host, int port)
{
    const QString trimmed = QString::fromStdString(host).trimmed();
    const QString newHost = trimmed.isEmpty() ? QStringLiteral("127.0.0.1") : trimmed;
    const quint16 newPort = (port >= 1 && port <= 65535) ? static_cast<quint16>(port) : 8766;

    if (m_remoteHost == newHost && m_remotePort == newPort) {
        return;
    }

    if (m_remoteMode && is_connected()) {
        disconnect_serial();
    }
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.abort();
        m_socketReadBuffer.clear();
    }
    m_remoteHost = newHost;
    m_remotePort = newPort;
}

std::string SerialWorker::remoteHost() const
{
    return m_remoteHost.toStdString();
}

int SerialWorker::remotePort() const
{
    return static_cast<int>(m_remotePort);
}

bool SerialWorker::is_connected() const
{
    return m_remoteMode ? m_remoteStageConnected : m_serial.isOpen();
}

std::string SerialWorker::current_port_name() const
{
    return m_currentPortName.toStdString();
}

bool SerialWorker::ensureRemoteSocketConnected(int timeoutMs)
{
    if (m_socket.state() == QAbstractSocket::ConnectedState) {
        return true;
    }
    if (m_socket.state() == QAbstractSocket::ConnectingState ||
        m_socket.state() == QAbstractSocket::HostLookupState) {
        if (m_socket.waitForConnected(timeoutMs)) {
            m_socket.setSocketOption(QAbstractSocket::LowDelayOption, 1);
            return true;
        }
    }
    m_socket.abort();
    m_socketReadBuffer.clear();
    m_socket.connectToHost(m_remoteHost, m_remotePort);
    if (!m_socket.waitForConnected(timeoutMs)) {
        return false;
    }
    m_socket.setSocketOption(QAbstractSocket::LowDelayOption, 1);
    return true;
}

void SerialWorker::drainRemotePackets()
{
    if (m_socket.bytesAvailable() > 0) {
        m_socketReadBuffer.append(m_socket.readAll());
    }

    while (m_socketReadBuffer.size() >= CameraNet::kPacketHeaderSize) {
        const uchar *hdr = reinterpret_cast<const uchar *>(m_socketReadBuffer.constData());
        const quint8 packetType = hdr[0];
        const quint32 payloadLen = qFromBigEndian<quint32>(hdr + 1);

        if (payloadLen > 16U * 1024U * 1024U) {
            m_socket.abort();
            m_socketReadBuffer.clear();
            return;
        }

        const int totalSize = CameraNet::kPacketHeaderSize + static_cast<int>(payloadLen);
        if (m_socketReadBuffer.size() < totalSize) {
            break;
        }

        const QByteArray payload = m_socketReadBuffer.mid(CameraNet::kPacketHeaderSize,
                                                          static_cast<int>(payloadLen));
        m_socketReadBuffer.remove(0, totalSize);

        if (packetType == CameraNet::kPacketTypeJson) {
            const QJsonDocument doc = QJsonDocument::fromJson(payload);
            if (!doc.isObject()) {
                continue;
            }
            const QJsonObject obj = doc.object();
            if (obj.contains(QStringLiteral("event"))) {
                m_pendingRemoteEvents.append(obj);
            } else if (obj.contains(QStringLiteral("id"))) {
                const qint64 respId = obj.value(QStringLiteral("id")).toInteger(0);
                m_lastResponseById.insert(respId, obj);
            }
        }
    }

    if (!m_inSyncRequest) {
        dispatchRemoteEvents();
    }
}

void SerialWorker::dispatchRemoteEvents()
{
    while (!m_pendingRemoteEvents.isEmpty()) {
        const QJsonObject ev = m_pendingRemoteEvents.takeFirst();
        const QString evType = ev.value(QStringLiteral("event")).toString();
        if (evType == QStringLiteral("stage_log")) {
            const QString msg = ev.value(QStringLiteral("message")).toString();
            if (!msg.isEmpty()) {
                emitLog(msg);
            }
        } else if (evType == QStringLiteral("stage_status")) {
            const bool connected = ev.value(QStringLiteral("connected")).toBool(false);
            const QString portName = ev.value(QStringLiteral("port_name")).toString();
            if (!portName.isEmpty()) {
                m_currentPortName = portName;
            }
            if (m_remoteStageConnected != connected) {
                m_remoteStageConnected = connected;
                emitStatus(connected);
            }
        } else if (evType == QStringLiteral("mcp_action")) {
            // Only stage actions belong in the CNC log window; camera actions are
            // reported by the camera client.
            if (ev.value(QStringLiteral("kind")).toString() == QStringLiteral("stage")) {
                const QString msg = ev.value(QStringLiteral("message")).toString();
                if (!msg.isEmpty()) {
                    emitLog(QStringLiteral("[MCP] ") + msg);
                }
            }
        }
    }
}

bool SerialWorker::sendRemoteRequest(const QString& cmd,
                                     const QJsonObject& params,
                                     QJsonObject* resultOut,
                                     QString* errorOut,
                                     int timeoutMs)
{
    if (!ensureRemoteSocketConnected(timeoutMs)) {
        if (errorOut) {
            *errorOut = QString("Could not connect to camera_server at %1:%2 (%3)")
                            .arg(m_remoteHost)
                            .arg(m_remotePort)
                            .arg(m_socket.errorString());
        }
        return false;
    }

    const qint64 reqId = m_nextReqId++;
    QJsonObject req;
    req[QStringLiteral("id")] = reqId;
    req[QStringLiteral("cmd")] = cmd;
    req[QStringLiteral("params")] = params;

    m_inSyncRequest = true;
    const QByteArray packet = CameraNet::packJsonPacket(req);
    if (m_socket.write(packet) != packet.size()) {
        m_inSyncRequest = false;
        if (errorOut) *errorOut = m_socket.errorString();
        return false;
    }
    m_socket.flush();

    QElapsedTimer timer;
    timer.start();
    while (!m_lastResponseById.contains(reqId) && timer.elapsed() < timeoutMs) {
        if (m_socket.state() != QAbstractSocket::ConnectedState) {
            break;
        }
        const int rem = std::max(10, timeoutMs - static_cast<int>(timer.elapsed()));
        m_socket.waitForReadyRead(std::min(50, rem));
        drainRemotePackets();
    }
    m_inSyncRequest = false;

    if (!m_lastResponseById.contains(reqId)) {
        dispatchRemoteEvents();
        if (errorOut) {
            *errorOut = QString("Timed out waiting for '%1' response from %2:%3")
                            .arg(cmd, m_remoteHost)
                            .arg(m_remotePort);
        }
        return false;
    }

    const QJsonObject resp = m_lastResponseById.take(reqId);
    const bool ok = resp.value(QStringLiteral("ok")).toBool(false);
    if (resultOut && resp.value(QStringLiteral("result")).isObject()) {
        *resultOut = resp.value(QStringLiteral("result")).toObject();
    }
    if (!ok && errorOut) {
        *errorOut = resp.value(QStringLiteral("error")).toString(QStringLiteral("Remote command failed"));
    }
    return ok;
}

std::vector<std::string> SerialWorker::available_ports(std::string* active_port_out,
                                                       bool* active_connected_out)
{
    std::vector<std::string> ports;
    if (active_port_out) active_port_out->clear();
    if (active_connected_out) *active_connected_out = false;

    if (!m_remoteMode) {
        const auto infos = QSerialPortInfo::availablePorts();
        for (const auto &port : infos) {
            ports.push_back(port.portName().toStdString());
        }
        if (active_port_out) *active_port_out = m_currentPortName.toStdString();
        if (active_connected_out) *active_connected_out = m_serial.isOpen();
        return ports;
    }

    QJsonObject resultObj;
    QString errMsg;
    if (sendRemoteRequest(QStringLiteral("stage_list_ports"), QJsonObject(), &resultObj, &errMsg, 600)) {
        const QJsonObject stageObj = resultObj.value(QStringLiteral("stage")).toObject();
        const QJsonArray portsArr = stageObj.value(QStringLiteral("ports")).toArray();
        for (const QJsonValue &v : portsArr) {
            const QString p = v.toString().trimmed();
            if (!p.isEmpty()) {
                ports.push_back(p.toStdString());
            }
        }
        if (active_port_out) {
            *active_port_out = stageObj.value(QStringLiteral("port_name")).toString().toStdString();
        }
        if (active_connected_out) {
            *active_connected_out = stageObj.value(QStringLiteral("connected")).toBool(false);
        }
    }
    dispatchRemoteEvents();
    return ports;
}

void SerialWorker::connect_serial(const std::string& port_name, int baud_rate)
{
    if (m_remoteMode) {
        QJsonObject params;
        params[QStringLiteral("port_name")] = QString::fromStdString(port_name);
        params[QStringLiteral("baud_rate")] = baud_rate;

        QJsonObject resultObj;
        QString errMsg;
        const bool ok = sendRemoteRequest(
            QStringLiteral("stage_connect"), params, &resultObj, &errMsg, 2000);

        if (ok) {
            const QJsonObject stageObj = resultObj.value(QStringLiteral("stage")).toObject();
            m_currentPortName = stageObj.value(QStringLiteral("port_name"))
                                    .toString(QString::fromStdString(port_name));
            m_remoteStageConnected = true;
            emitStatus(true);
            dispatchRemoteEvents();
        } else {
            m_remoteStageConnected = false;
            dispatchRemoteEvents();
            emitLog(QString("Failed to connect to remote stage (%1:%2): %3")
                        .arg(m_remoteHost)
                        .arg(m_remotePort)
                        .arg(errMsg));
            emitStatus(false);
        }
        return;
    }

    if (m_serial.isOpen()) {
        m_serial.close();
    }

    m_serial.setPortName(QString::fromStdString(port_name));
    m_serial.setBaudRate(baud_rate);
    m_serial.setDataBits(QSerialPort::Data8);
    m_serial.setParity(QSerialPort::NoParity);
    m_serial.setStopBits(QSerialPort::OneStop);
    m_serial.setFlowControl(QSerialPort::NoFlowControl);

    if (m_serial.open(QIODevice::ReadWrite)) {
        m_currentPortName = QString::fromStdString(port_name);
        emitLog(QString::fromStdString(
            "Connected to " + port_name + " at " + std::to_string(baud_rate) + " baud."));
        emitStatus(true);
    } else {
        emitLog("Failed to connect to " + QString::fromStdString(port_name) + ": " + m_serial.errorString());
        emitStatus(false);
    }
}

void SerialWorker::disconnect_serial()
{
    if (m_remoteMode) {
        if (m_remoteStageConnected) {
            if (m_socket.state() == QAbstractSocket::ConnectedState) {
                QJsonObject resultObj;
                QString errMsg;
                sendRemoteRequest(QStringLiteral("stage_disconnect"), QJsonObject(), &resultObj, &errMsg, 500);
            }
            m_remoteStageConnected = false;
            dispatchRemoteEvents();
            emitStatus(false);
        }
        return;
    }

    if (m_serial.isOpen()) {
        m_serial.close();
        emitLog("Disconnected from serial port.");
        emitStatus(false);
    }
}

bool SerialWorker::send_command(const std::string& cmd)
{
    if (m_remoteMode) {
        if (!m_remoteStageConnected || m_socket.state() != QAbstractSocket::ConnectedState) {
            emitLog("Error: Remote stage serial port not open.");
            return false;
        }
        QJsonObject params;
        params[QStringLiteral("command")] = QString::fromStdString(cmd);
        QJsonObject req;
        req[QStringLiteral("id")] = m_nextReqId++;
        req[QStringLiteral("cmd")] = QStringLiteral("stage_send_command");
        req[QStringLiteral("params")] = params;

        const QByteArray packet = CameraNet::packJsonPacket(req);
        if (m_socket.write(packet) != packet.size()) {
            emitLog("Error sending command '" + QString::fromStdString(cmd) +
                    "': " + m_socket.errorString());
            return false;
        }
        m_socket.flush();
        emitLog("> " + QString::fromStdString(cmd));
        return true;
    }

    if (m_serial.isOpen()) {
        std::string full_cmd = cmd + "\n";
        const qint64 written = m_serial.write(full_cmd.c_str(), full_cmd.size());
        if (written != static_cast<qint64>(full_cmd.size())) {
            const QString error = written < 0
                ? m_serial.errorString()
                : QString("short write (%1/%2 bytes)")
                      .arg(written)
                      .arg(full_cmd.size());
            emitLog("Error sending command '" + QString::fromStdString(cmd) + "': " + error);
            return false;
        }

        m_serial.flush();
        emitLog("> " + QString::fromStdString(cmd));
        return true;
    } else {
        emitLog("Error: Serial port not open.");
        return false;
    }
}

bool SerialWorker::send_raw_command(const std::string& cmd)
{
    if (m_remoteMode) {
        if (!m_remoteStageConnected || m_socket.state() != QAbstractSocket::ConnectedState) {
            return false;
        }
        QJsonObject params;
        const QByteArray rawBytes(cmd.data(), static_cast<qsizetype>(cmd.size()));
        params[QStringLiteral("data_hex")] = QString::fromLatin1(rawBytes.toHex());
        QJsonObject req;
        req[QStringLiteral("id")] = m_nextReqId++;
        req[QStringLiteral("cmd")] = QStringLiteral("stage_send_raw");
        req[QStringLiteral("params")] = params;

        const QByteArray packet = CameraNet::packJsonPacket(req);
        if (m_socket.write(packet) != packet.size()) {
            return false;
        }
        m_socket.flush();
        return true;
    }

    if (m_serial.isOpen()) {
        const qint64 written = m_serial.write(cmd.c_str(), cmd.size());
        if (written != static_cast<qint64>(cmd.size())) {
            return false;
        }
        m_serial.flush();
        return true;
    }

    return false;
}

void SerialWorker::poll_serial()
{
    if (m_remoteMode && m_socket.state() == QAbstractSocket::ConnectedState) {
        drainRemotePackets();
    }
}

void SerialWorker::handleReadyRead()
{
    while (m_serial.canReadLine()) {
        QByteArray line = m_serial.readLine();
        QString s = QString::fromLatin1(line).trimmed();
        if (!s.isEmpty()) {
            emitLog("Rx: " + s);
        }
    }
}

void SerialWorker::handleError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::ResourceError) {
        emitLog("Serial port resource error: " + m_serial.errorString());
        disconnect_serial();
    }
}

void SerialWorker::handleSocketReadyRead()
{
    drainRemotePackets();
}

void SerialWorker::handleSocketDisconnected()
{
    if (m_remoteMode && m_remoteStageConnected) {
        m_remoteStageConnected = false;
        emitLog("Disconnected from remote stage server.");
        emitStatus(false);
    }
}
