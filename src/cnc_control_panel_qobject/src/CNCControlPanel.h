#ifndef CNCCONTROLPANEL_H
#define CNCCONTROLPANEL_H

#include <QGroupBox>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QTimer>
#include <QStringList>

#include "SerialWorker.h"

class CNCControlPanel : public QGroupBox
{
    Q_OBJECT
public:
    explicit CNCControlPanel(QWidget *parent = nullptr);
    ~CNCControlPanel() override;

    void stop();
    int feedrate() const { return m_xyFeedrate; }

signals:
    void logSignal(const QString &msg);
    void stateUpdated(const QString &state);
    void positionUpdated(double x, double y, double z);
    void scanStartReady();
    void scanRowStartReady();
    void scanRowReady();
    void scanFinished();

public slots:
    void sendCommand(const QString &cmd);
    void moveUp();
    void moveDown();
    void moveLeft();
    void moveRight();
    void moveForward();
    void moveBack();

private slots:
    void refreshSerialPorts();
    void onConnectToggled(bool checked);
    void onLogMessage(const std::string &msg);
    void onSerialStatusChanged(bool connected);
    void enqueueCommand(const QString &cmd);
    void processQueue();
    void pollStatus();
    void home();
    void resetCnc();
    void rebootCnc();
    void sendConsoleCommand();
    void onStepSizeChanged(double v);
    void onZStepSizeChanged(double v);
    void onXYFeedrateChanged(int v);
    void onZFeedrateChanged(int v);

private:
    enum class PendingScanEvent {
        None,
        RowStart,
        RowReady,
        ScanFinished
    };

    void setupUi();
    void parseStatus(const QString &statusStr);
    bool flushPendingScanEventIfIdle();

    SerialWorker *m_serialWorker = nullptr;
    QTimer m_serialPollTimer;
    QTimer m_statusPollTimer;

    QComboBox *m_serialPortCombo = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_forwardButton = nullptr;
    QPushButton *m_backButton = nullptr;
    QPushButton *m_leftButton = nullptr;
    QPushButton *m_rightButton = nullptr;
    QPushButton *m_upButton = nullptr;
    QPushButton *m_downButton = nullptr;
    QPushButton *m_homeButton = nullptr;
    QPushButton *m_resetButton = nullptr;
    QPushButton *m_rebootButton = nullptr;
    QDoubleSpinBox *m_stepInput = nullptr;
    QDoubleSpinBox *m_zStepInput = nullptr;
    QSpinBox *m_xyFeedrateInput = nullptr;
    QSpinBox *m_zFeedrateInput = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_wposXLabel = nullptr;
    QLabel *m_wposYLabel = nullptr;
    QLabel *m_wposZLabel = nullptr;
    QPlainTextEdit *m_logWindow = nullptr;
    QLineEdit *m_commandInput = nullptr;
    QPushButton *m_sendCommandButton = nullptr;

    QStringList m_commandQueue;
    bool m_waitingForOk = false;
    QString m_lastSentCommand;
    PendingScanEvent m_pendingScanEvent = PendingScanEvent::None;
    QString m_lastState;
    double m_stepSize = 0.1;
    double m_zStepSize = 0.01;
    int m_xyFeedrate = 500;
    int m_zFeedrate = 100;
};

#endif // CNCCONTROLPANEL_H
