#ifndef SERIALWORKER_H
#define SERIALWORKER_H

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSerialPort>
#include <QString>
#include <QTcpSocket>
#include <functional>
#include <string>
#include <vector>

class SerialWorker : public QObject {
    Q_OBJECT
public:
    explicit SerialWorker(QObject *parent = nullptr);
    ~SerialWorker() override;

    void register_log_callback(std::function<void(std::string)> cb);
    void register_status_callback(std::function<void(bool)> cb);

    // Remote TCP stage mode (connects to MindVisionCameraServer / camera_server)
    void setRemoteMode(bool enabled);
    bool isRemoteMode() const;
    void setRemoteEndpoint(const std::string& host, int port = 8766);
    std::string remoteHost() const;
    int remotePort() const;

    std::vector<std::string> available_ports(std::string* active_port_out = nullptr,
                                             bool* active_connected_out = nullptr);
    bool is_connected() const;
    std::string current_port_name() const;

    void connect_serial(const std::string& port_name, int baud_rate);
    void disconnect_serial();
    bool send_command(const std::string& cmd);
    bool send_raw_command(const std::string& cmd);
    void poll_serial();

signals:
    void log_signal(const QString& msg);
    void connection_status(bool connected);

private slots:
    void handleReadyRead();
    void handleError(QSerialPort::SerialPortError error);
    void handleSocketReadyRead();
    void handleSocketDisconnected();

private:
    void emitLog(const QString& msg);
    void emitStatus(bool connected);
    bool ensureRemoteSocketConnected(int timeoutMs = 800);
    void drainRemotePackets();
    void dispatchRemoteEvents();
    bool sendRemoteRequest(const QString& cmd,
                           const QJsonObject& params,
                           QJsonObject* resultOut = nullptr,
                           QString* errorOut = nullptr,
                           int timeoutMs = 1200);

    QSerialPort m_serial;
    QTcpSocket m_socket;
    QByteArray m_socketReadBuffer;
    bool m_remoteMode = false;
    QString m_remoteHost = QStringLiteral("127.0.0.1");
    quint16 m_remotePort = 8766;
    bool m_remoteStageConnected = false;
    QString m_currentPortName;
    qint64 m_nextReqId = 1;
    bool m_inSyncRequest = false;
    QHash<qint64, QJsonObject> m_lastResponseById;
    QList<QJsonObject> m_pendingRemoteEvents;

    std::function<void(std::string)> m_log_cb;
    std::function<void(bool)> m_status_cb;
};

#endif // SERIALWORKER_H
