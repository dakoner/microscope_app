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
#include <QElapsedTimer>
#include <QStringList>
#include <QList>
#include <QVector>
#include <limits>

#include "SerialWorker.h"

class CNCControlPanel : public QGroupBox
{
    Q_OBJECT
public:
    explicit CNCControlPanel(QWidget *parent = nullptr);
    ~CNCControlPanel() override;

    void stop();
    void setStatusPollIntervalMs(int intervalMs);
    int feedrate() const { return m_xyFeedrate; }

    // Remote stage mode (connects to camera_server)
    void setRemoteMode(bool remote);
    bool isRemoteMode() const;
    void setRemoteEndpoint(const QString &host, quint16 port = 8766);
    QString remoteHost() const;
    quint16 remotePort() const;
    bool isStageConnected() const;
    // True once the controller has actually answered a status request.  A serial
    // port can be open and writable while the controller is silent or wedged, so
    // isStageConnected() alone is not enough to trust motion commands.
    bool hasControllerStatus() const { return !m_lastState.isEmpty(); }
    QString currentPortName() const;
    void setPreferredPort(const QString &portName);
    bool connectStage(const QString &portName = QString(), int baudRate = 115200);
    void disconnectStage();

    // Closed-loop autofocus.  The panel drives the Z axis and the host window
    // feeds it one sharpness measurement per camera frame, so the objective never
    // has to cross into the motion layer.
    bool isAutofocusActive() const { return m_focusPhase != FocusPhase::Inactive; }
    double lastFocusScore() const { return m_lastFocusScore; }
    double currentZPositionMm() const { return m_lastZPosition; }
    // The host window blocks autofocus while a mosaic scan owns the stage.
    void setAutofocusAllowed(bool allowed);
    // Non-positive arguments keep the values the panel's own spin boxes show.
    bool startAutofocus(double searchRangeMm = 0.0, double coarseStepMm = 0.0);
    void cancelAutofocus(const QString &reason = QString());

signals:
    void logSignal(const QString &msg);
    void stateUpdated(const QString &state);
    void positionUpdated(double x, double y, double z);
    void scanStartReady();
    void scanRowStartReady();
    void scanRowReady();
    void scanFinished();
    void autofocusStarted();
    void autofocusFinished(bool success, double bestZMm, double bestScore, const QString &message);

public slots:
    void refreshSerialPorts();
    void sendCommand(const QString &cmd);
    void moveUp();
    void moveDown();
    void moveLeft();
    void moveRight();
    void moveForward();
    void moveBack();
    // One sharpness measurement per camera frame, pushed by the host window.
    void submitFocusSample(double score);

private slots:
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
    void onAutofocusClicked();
    void advanceAutofocus();

private:
    enum class PendingScanEvent {
        None,
        RowStart,
        RowReady,
        ScanFinished
    };

    // Move-and-measure states of the autofocus search.
    enum class FocusPhase {
        Inactive,
        Move,    // a Z jog is in flight, then waiting for the settle
        Sample   // averaging pushed sharpness measurements
    };

    void setupUi();
    void parseStatus(const QString &statusStr);
    bool flushPendingScanEventIfIdle();
    void updateAutofocusControlsEnabled();
    void beginFocusMove();
    void beginFocusSampleWindow();
    void finishFocusSample();
    void startFocusPass(int pass, double centreMm, double halfWidthMm, double stepMm);
    void finishAutofocus(bool success, const QString &message);

    SerialWorker *m_serialWorker = nullptr;
    QTimer m_serialPollTimer;
    QTimer m_statusPollTimer;
    int m_statusPollIntervalMs = 100;

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
    QList<PendingScanEvent> m_pendingScanEvents;
    QString m_lastState;
    QString m_preferredPort;
    double m_stepSize = 0.1;
    double m_zStepSize = 0.01;
    int m_xyFeedrate = 500;
    int m_zFeedrate = 100;

    // Autofocus
    QPushButton *m_autofocusButton = nullptr;
    QDoubleSpinBox *m_focusRangeInput = nullptr;
    QDoubleSpinBox *m_focusStepInput = nullptr;
    QLabel *m_focusScoreLabel = nullptr;
    QTimer m_focusTimer;
    QElapsedTimer m_focusClock;
    FocusPhase m_focusPhase = FocusPhase::Inactive;
    bool m_autofocusAllowed = true;
    int m_focusPass = 0;
    int m_focusIndex = 0;
    QVector<double> m_focusTargets;
    QVector<double> m_focusScores;
    double m_focusTargetZ = 0.0;
    double m_focusMoveWaitMs = 0.0;
    double m_focusCoarseStepMm = 0.05;
    double m_focusFineStepMm = 0.0125;
    int m_focusFeedrate = 100;
    double m_focusScoreSum = 0.0;
    int m_focusScoreCount = 0;
    double m_lastFocusScore = 0.0;
    double m_focusBestZ = 0.0;
    double m_focusBestScore = 0.0;
    double m_lastZPosition = std::numeric_limits<double>::quiet_NaN();
};

#endif // CNCCONTROLPANEL_H
