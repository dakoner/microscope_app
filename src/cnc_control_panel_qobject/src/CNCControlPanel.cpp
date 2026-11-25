#include "CNCControlPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QSerialPortInfo>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

static QString normalizeRxContent(const QString &content)
{
    QString result;
    for (const QChar &ch : content) {
        if (ch >= ' ' && ch <= '~')
            result.append(ch);
    }
    return result.trimmed();
}

enum RxKind { RxNone, RxStatus, RxOk, RxError };

// Autofocus timing.  Each search point is a small Z jog followed by a settle and
// a window in which the pushed sharpness measurements are averaged, so a single
// noisy frame can never steer the loop.
namespace {
constexpr int kFocusTickMs = 40;
constexpr int kFocusSettleMs = 250;
constexpr int kFocusSampleMs = 300;
constexpr int kFocusMoveTimeoutMs = 8000;
constexpr double kFocusFineDivisor = 4.0;
constexpr double kFocusTolerance = 1e-4;
}

static RxKind classifyRx(const QString &content, QString &normalized)
{
    normalized = normalizeRxContent(content);
    QString lowered = normalized.toLower();
    if (normalized.startsWith('<') && normalized.endsWith('>'))
        return RxStatus;
    static QRegularExpression okRe("(^|[^a-z])ok([^a-z]|$)");
    if (okRe.match(lowered).hasMatch())
        return RxOk;
    if (lowered.contains("error"))
        return RxError;
    return RxNone;
}

CNCControlPanel::CNCControlPanel(QWidget *parent)
    : QGroupBox("CNC Control", parent)
{
    m_serialWorker = new SerialWorker(this);
    setupUi();

    m_serialWorker->register_log_callback([this](std::string msg) {
        QMetaObject::invokeMethod(this, [this, msg = QString::fromStdString(msg)]() {
            onLogMessage(msg.toStdString());
        }, Qt::QueuedConnection);
    });
    m_serialWorker->register_status_callback([this](bool connected) {
        QMetaObject::invokeMethod(this, [this, connected]() {
            onSerialStatusChanged(connected);
        }, Qt::QueuedConnection);
    });

    connect(&m_serialPollTimer, &QTimer::timeout, this, [this]() {
        m_serialWorker->poll_serial();
    });
    m_serialPollTimer.start(50);

    connect(&m_statusPollTimer, &QTimer::timeout, this, &CNCControlPanel::pollStatus);

    connect(&m_focusTimer, &QTimer::timeout, this, &CNCControlPanel::advanceAutofocus);

    m_jogDispatchTimer.setSingleShot(true);
    connect(&m_jogDispatchTimer, &QTimer::timeout, this, &CNCControlPanel::dispatchContinuousJog);
    connect(&m_jogContinueTimer, &QTimer::timeout, this, &CNCControlPanel::checkContinuousJogContinuation);
    connect(&m_jogStopTimer, &QTimer::timeout, this, &CNCControlPanel::retryPendingJogStop);

    refreshSerialPorts();
    onSerialStatusChanged(false);
}

CNCControlPanel::~CNCControlPanel()
{
    stop();
}

void CNCControlPanel::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);

    // Serial connection row
    auto *connLayout = new QHBoxLayout;
    m_serialPortCombo = new QComboBox;
    m_refreshButton = new QPushButton("Refresh");
    m_connectButton = new QPushButton("Connect");
    m_connectButton->setCheckable(true);
    connLayout->addWidget(m_serialPortCombo);
    connLayout->addWidget(m_refreshButton);
    connLayout->addWidget(m_connectButton);
    mainLayout->addLayout(connLayout);

    // Status
    auto *statusLayout = new QFormLayout;
    m_statusLabel = new QLabel("N/A");
    m_wposXLabel = new QLabel("0.000");
    m_wposYLabel = new QLabel("0.000");
    m_wposZLabel = new QLabel("0.000");
    statusLayout->addRow("State:", m_statusLabel);
    auto *posRow = new QHBoxLayout;
    posRow->addWidget(new QLabel("X:"));
    posRow->addWidget(m_wposXLabel);
    posRow->addWidget(new QLabel("Y:"));
    posRow->addWidget(m_wposYLabel);
    posRow->addWidget(new QLabel("Z:"));
    posRow->addWidget(m_wposZLabel);
    statusLayout->addRow("WPos:", posRow);
    mainLayout->addLayout(statusLayout);

    // Closed-loop autofocus.  Placed directly under the position readout so the
    // main control stays visible without scrolling the panel.
    auto *focusLayout = new QHBoxLayout;
    m_autofocusButton = new QPushButton("Autofocus");
    m_autofocusButton->setToolTip(QStringLiteral(
        "Sweep the Z axis across the search range, measure sharpness at every point, and\n"
        "return the stage to the best focus found.  Travels at the Z feedrate set below."));
    m_focusRangeInput = new QDoubleSpinBox;
    m_focusRangeInput->setRange(0.01, 10.0);
    m_focusRangeInput->setDecimals(3);
    m_focusRangeInput->setValue(1.0);
    m_focusRangeInput->setPrefix("Range: ");
    m_focusRangeInput->setSuffix(" mm");
    m_focusRangeInput->setToolTip(QStringLiteral(
        "Total Z travel to search, centred on the current position."));
    m_focusStepInput = new QDoubleSpinBox;
    m_focusStepInput->setRange(0.001, 1.0);
    m_focusStepInput->setDecimals(3);
    m_focusStepInput->setValue(0.05);
    m_focusStepInput->setPrefix("Step: ");
    m_focusStepInput->setSuffix(" mm");
    m_focusStepInput->setToolTip(QStringLiteral(
        "Coarse search step.  The refinement pass searches at a quarter of this step."));
    m_focusScoreLabel = new QLabel(QStringLiteral("Focus: --"));
    m_focusScoreLabel->setToolTip(QStringLiteral("Live sharpness score of the current camera frame."));
    focusLayout->addWidget(m_autofocusButton);
    focusLayout->addWidget(m_focusRangeInput);
    focusLayout->addWidget(m_focusStepInput);
    focusLayout->addStretch();
    focusLayout->addWidget(m_focusScoreLabel);
    mainLayout->addLayout(focusLayout);

    // Movement grid
    auto *grid = new QGridLayout;
    m_forwardButton = new QPushButton("Y+");
    m_backButton = new QPushButton("Y-");
    m_leftButton = new QPushButton("X-");
    m_rightButton = new QPushButton("X+");
    m_upButton = new QPushButton("Z+");
    m_downButton = new QPushButton("Z-");
    grid->addWidget(m_forwardButton, 0, 1);
    grid->addWidget(m_leftButton, 1, 0);
    grid->addWidget(m_rightButton, 1, 2);
    grid->addWidget(m_backButton, 2, 1);
    grid->addWidget(m_upButton, 0, 3);
    grid->addWidget(m_downButton, 2, 3);
    mainLayout->addLayout(grid);

    // Step sizes
    auto *stepLayout = new QHBoxLayout;
    m_stepInput = new QDoubleSpinBox;
    m_stepInput->setRange(0.001, 100.0);
    m_stepInput->setDecimals(3);
    m_stepInput->setValue(m_stepSize);
    m_stepInput->setPrefix("XY: ");
    m_stepInput->setSuffix(" mm");
    m_zStepInput = new QDoubleSpinBox;
    m_zStepInput->setRange(0.001, 10.0);
    m_zStepInput->setDecimals(3);
    m_zStepInput->setValue(m_zStepSize);
    m_zStepInput->setPrefix("Z: ");
    m_zStepInput->setSuffix(" mm");
    stepLayout->addWidget(m_stepInput);
    stepLayout->addWidget(m_zStepInput);
    mainLayout->addLayout(stepLayout);

    // Feedrates
    auto *feedLayout = new QHBoxLayout;
    m_xyFeedrateInput = new QSpinBox;
    m_xyFeedrateInput->setRange(1, 10000);
    m_xyFeedrateInput->setValue(m_xyFeedrate);
    m_xyFeedrateInput->setPrefix("XY F: ");
    m_zFeedrateInput = new QSpinBox;
    m_zFeedrateInput->setRange(1, 5000);
    m_zFeedrateInput->setValue(m_zFeedrate);
    m_zFeedrateInput->setPrefix("Z F: ");
    feedLayout->addWidget(m_xyFeedrateInput);
    feedLayout->addWidget(m_zFeedrateInput);
    mainLayout->addLayout(feedLayout);

    // Keyboard Hold-to-Jog controls
    auto *jogModeLayout = new QHBoxLayout;
    m_holdToJogCheck = new QCheckBox(QStringLiteral("Hold-to-Jog"));
    m_holdToJogCheck->setChecked(true);
    m_holdToJogCheck->setToolTip(QStringLiteral(
        "Hold Arrows/WASD (XY) or PageUp/PageDown/E/Q (Z) for smooth continuous jogging.\n"
        "Hold Shift for fine speed; release key or press Esc to decelerate smoothly to a stop.\n"
        "Uncheck to use discrete single-step taps per key press."));
    m_jogSpeedCombo = new QComboBox;
    m_jogSpeedCombo->addItem(QStringLiteral("Fine (0.2x)"), 0.2);
    m_jogSpeedCombo->addItem(QStringLiteral("Normal (1x)"), 1.0);
    m_jogSpeedCombo->addItem(QStringLiteral("Fast (2x)"), 2.0);
    m_jogSpeedCombo->setCurrentIndex(1);
    m_jogSpeedCombo->setToolTip(QStringLiteral("Speed preset for continuous keyboard Hold-to-Jog."));
    jogModeLayout->addWidget(m_holdToJogCheck);
    jogModeLayout->addWidget(m_jogSpeedCombo);
    mainLayout->addLayout(jogModeLayout);

    // Action buttons
    auto *actLayout = new QHBoxLayout;
    m_homeButton = new QPushButton("Home");
    m_resetButton = new QPushButton("Reset");
    m_rebootButton = new QPushButton("Reboot");
    actLayout->addWidget(m_homeButton);
    actLayout->addWidget(m_resetButton);
    actLayout->addWidget(m_rebootButton);
    mainLayout->addLayout(actLayout);

    // Console command
    auto *cmdLayout = new QHBoxLayout;
    m_commandInput = new QLineEdit;
    m_commandInput->setPlaceholderText("G-code command");
    m_sendCommandButton = new QPushButton("Send");
    cmdLayout->addWidget(m_commandInput);
    cmdLayout->addWidget(m_sendCommandButton);
    mainLayout->addLayout(cmdLayout);

    // CNC log window
    m_logWindow = new QPlainTextEdit;
    m_logWindow->setReadOnly(true);
    m_logWindow->setMaximumBlockCount(1000);
    mainLayout->addWidget(m_logWindow);

    // Connections
    connect(m_refreshButton, &QPushButton::clicked, this, &CNCControlPanel::refreshSerialPorts);
    connect(m_connectButton, &QPushButton::toggled, this, &CNCControlPanel::onConnectToggled);
    connect(m_forwardButton, &QPushButton::clicked, this, &CNCControlPanel::moveForward);
    connect(m_backButton, &QPushButton::clicked, this, &CNCControlPanel::moveBack);
    connect(m_leftButton, &QPushButton::clicked, this, &CNCControlPanel::moveLeft);
    connect(m_rightButton, &QPushButton::clicked, this, &CNCControlPanel::moveRight);
    connect(m_upButton, &QPushButton::clicked, this, &CNCControlPanel::moveUp);
    connect(m_downButton, &QPushButton::clicked, this, &CNCControlPanel::moveDown);
    connect(m_homeButton, &QPushButton::clicked, this, &CNCControlPanel::home);
    connect(m_resetButton, &QPushButton::clicked, this, &CNCControlPanel::resetCnc);
    connect(m_rebootButton, &QPushButton::clicked, this, &CNCControlPanel::rebootCnc);
    connect(m_stepInput, &QDoubleSpinBox::valueChanged, this, &CNCControlPanel::onStepSizeChanged);
    connect(m_zStepInput, &QDoubleSpinBox::valueChanged, this, &CNCControlPanel::onZStepSizeChanged);
    connect(m_xyFeedrateInput, &QSpinBox::valueChanged, this, &CNCControlPanel::onXYFeedrateChanged);
    connect(m_zFeedrateInput, &QSpinBox::valueChanged, this, &CNCControlPanel::onZFeedrateChanged);
    connect(m_sendCommandButton, &QPushButton::clicked, this, &CNCControlPanel::sendConsoleCommand);
    connect(m_commandInput, &QLineEdit::returnPressed, this, &CNCControlPanel::sendConsoleCommand);
    connect(m_autofocusButton, &QPushButton::clicked, this, &CNCControlPanel::onAutofocusClicked);
    connect(m_holdToJogCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (!checked) {
            stopContinuousJog();
        }
        if (m_jogSpeedCombo) {
            m_jogSpeedCombo->setEnabled(checked);
        }
    });
    connect(m_jogSpeedCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_continuousJogActive) {
            const int dx = m_jogDx;
            const int dy = m_jogDy;
            const int dz = m_jogDz;
            const bool fine = m_jogFineModifier;
            sendJogCancel();
            m_continuousJogActive = false;
            m_jogDx = dx;
            m_jogDy = dy;
            m_jogDz = dz;
            m_jogFineModifier = fine;
            m_jogDispatchTimer.start(25);
        }
    });
}

void CNCControlPanel::setRemoteMode(bool remote)
{
    if (!m_serialWorker) {
        return;
    }
    const bool changed = (m_serialWorker->isRemoteMode() != remote);
    m_serialWorker->setRemoteMode(remote);
    if (remote) {
        setTitle(QString("CNC Control (Remote %1:%2)")
                     .arg(QString::fromStdString(m_serialWorker->remoteHost()))
                     .arg(m_serialWorker->remotePort()));
    } else {
        setTitle(QStringLiteral("CNC Control"));
    }
    if (changed) {
        refreshSerialPorts();
    }
}

bool CNCControlPanel::isRemoteMode() const
{
    return m_serialWorker && m_serialWorker->isRemoteMode();
}

void CNCControlPanel::setRemoteEndpoint(const QString &host, quint16 port)
{
    if (!m_serialWorker) {
        return;
    }
    m_serialWorker->setRemoteEndpoint(host.toStdString(), static_cast<int>(port));
    if (m_serialWorker->isRemoteMode()) {
        setTitle(QString("CNC Control (Remote %1:%2)")
                     .arg(QString::fromStdString(m_serialWorker->remoteHost()))
                     .arg(m_serialWorker->remotePort()));
    }
}

QString CNCControlPanel::remoteHost() const
{
    return m_serialWorker ? QString::fromStdString(m_serialWorker->remoteHost()) : QStringLiteral("127.0.0.1");
}

quint16 CNCControlPanel::remotePort() const
{
    return m_serialWorker ? static_cast<quint16>(m_serialWorker->remotePort()) : 8766;
}

bool CNCControlPanel::isStageConnected() const
{
    return m_serialWorker && m_serialWorker->is_connected();
}

QString CNCControlPanel::currentPortName() const
{
    if (m_serialWorker && m_serialWorker->is_connected()) {
        const QString p = QString::fromStdString(m_serialWorker->current_port_name());
        if (!p.isEmpty()) {
            return p;
        }
    }
    return m_serialPortCombo ? m_serialPortCombo->currentText() : QString();
}

void CNCControlPanel::setPreferredPort(const QString &portName)
{
    m_preferredPort = portName.trimmed();
    if (!m_preferredPort.isEmpty() && m_serialPortCombo) {
        const int idx = m_serialPortCombo->findText(m_preferredPort);
        if (idx >= 0) {
            m_serialPortCombo->setCurrentIndex(idx);
        }
    }
}

bool CNCControlPanel::connectStage(const QString &portName, int baudRate)
{
    if (!m_serialWorker) {
        return false;
    }
    QString target = portName.trimmed();
    if (target.isEmpty() && m_serialPortCombo) {
        target = m_serialPortCombo->currentText().trimmed();
    }
    if (target.isEmpty() && !m_preferredPort.isEmpty()) {
        target = m_preferredPort;
    }
    if (target.isEmpty()) {
        refreshSerialPorts();
        if (m_serialPortCombo) {
            target = m_serialPortCombo->currentText().trimmed();
        }
    }
    if (target.isEmpty()) {
        return false;
    }
    if (m_serialPortCombo) {
        int idx = m_serialPortCombo->findText(target);
        if (idx < 0) {
            m_serialPortCombo->addItem(target);
            idx = m_serialPortCombo->findText(target);
        }
        if (idx >= 0) {
            m_serialPortCombo->setCurrentIndex(idx);
        }
    }
    m_serialWorker->connect_serial(target.toStdString(), baudRate);
    return m_serialWorker->is_connected();
}

void CNCControlPanel::disconnectStage()
{
    if (m_serialWorker) {
        m_serialWorker->disconnect_serial();
    }
}

void CNCControlPanel::refreshSerialPorts()
{
    if (!m_serialPortCombo || !m_serialWorker) {
        return;
    }
    const QString prevSelection = m_serialPortCombo->currentText().trimmed();
    m_serialPortCombo->clear();

    std::string activePort;
    bool activeConnected = false;
    const auto ports = m_serialWorker->available_ports(&activePort, &activeConnected);
    for (const auto &port : ports) {
        m_serialPortCombo->addItem(QString::fromStdString(port));
    }

    QString target = QString::fromStdString(activePort).trimmed();
    if (target.isEmpty() && !m_preferredPort.isEmpty()) {
        target = m_preferredPort;
    }
    if (target.isEmpty()) {
        target = prevSelection;
    }
    if (!target.isEmpty()) {
        const int idx = m_serialPortCombo->findText(target);
        if (idx >= 0) {
            m_serialPortCombo->setCurrentIndex(idx);
        }
    }
}

void CNCControlPanel::onConnectToggled(bool checked)
{
    if (checked) {
        if (m_serialWorker && m_serialWorker->is_connected()) {
            return;
        }
        QString port = m_serialPortCombo->currentText();
        if (!port.isEmpty())
            m_serialWorker->connect_serial(port.toStdString(), 115200);
        else
            m_connectButton->setChecked(false);
    } else {
        if (m_serialWorker && m_serialWorker->is_connected()) {
            m_serialWorker->disconnect_serial();
        }
    }
}

void CNCControlPanel::onLogMessage(const std::string &msg)
{
    QString qmsg = QString::fromStdString(msg);

    if (qmsg.startsWith("Rx: ")) {
        QString rxContent = qmsg.mid(4);
        QString normalized;
        RxKind kind = classifyRx(rxContent, normalized);

        if (kind == RxStatus) {
            parseStatus(normalized.mid(1, normalized.size() - 2));
        } else if (kind == RxOk || kind == RxError) {
            if (m_logWindow)
                m_logWindow->appendPlainText(qmsg);

            m_waitingForOk = false;

            const bool wasJogCmd = m_lastSentCommand.startsWith(QLatin1String("$J="));
            if (m_lastSentCommand.contains("SCAN_START") && kind == RxOk) {
                emit scanStartReady();
                emit logSignal("Initial scan move complete.");
                m_lastSentCommand.clear();
            } else if (m_lastSentCommand.contains("SCAN_ROW_START") && kind == RxOk) {
                emit scanRowStartReady();
                m_lastSentCommand.clear();
            } else if (m_lastSentCommand.contains("SCAN_ROW_END") && kind == RxOk) {
                emit scanRowReady();
                m_lastSentCommand.clear();
            } else if (m_lastSentCommand.contains("SCAN_DONE")) {
                emit scanFinished();
                emit logSignal("Scan finished.");
                m_lastSentCommand.clear();
            } else if (wasJogCmd) {
                m_lastSentCommand.clear();
                // If the user already released the jog key before this $J= was
                // accepted by FluidNC, send 0x85 right now so the newly queued
                // jog is cancelled immediately instead of running away.
                if (m_jogStopPending && !m_continuousJogActive) {
                    sendJogCancel();
                }
            }

            processQueue();
            emit logSignal(qmsg);
        } else {
            if (m_logWindow)
                m_logWindow->appendPlainText(qmsg);

            emit logSignal(qmsg);
            if (qmsg.contains("Grbl 4.0 [FluidNC")) {
                QTimer::singleShot(500, this, [this]() {
                    sendCommand("$Report/Interval=10");
                });
            }
        }
    } else {
        if (m_logWindow)
            m_logWindow->appendPlainText(qmsg);

        emit logSignal(qmsg);
    }
}

void CNCControlPanel::parseStatus(const QString &statusStr)
{
    QStringList parts = statusStr.split('|');
    if (!parts.isEmpty()) {
        m_statusLabel->setText(parts[0]);
        emit stateUpdated(parts[0]);

        // Release internal scan markers once the controller is truly idle.
        if (parts[0] == "Idle" && flushPendingScanEventIfIdle()) {
            processQueue();
        }

        m_lastState = parts[0];

        // A controller that drops into Alarm or Check will not answer further
        // jogs, so end the search instead of letting it run into the timeout.
        if (m_focusPhase != FocusPhase::Inactive &&
            (parts[0] == QLatin1String("Alarm") || parts[0] == QLatin1String("Check"))) {
            cancelAutofocus(QString("Autofocus aborted: the controller reported %1.").arg(parts[0]));
        }
        if (m_continuousJogActive &&
            (parts[0] == QLatin1String("Jog") || parts[0] == QLatin1String("Run"))) {
            m_jogSawRunning = true;
        }
        if (m_jogStopPending) {
            if (parts[0] == QLatin1String("Jog")) {
                // Controller is still jogging after release: re-issue 0x85 immediately.
                sendJogCancel();
            } else if (parts[0] == QLatin1String("Idle") && !m_waitingForOk &&
                       m_jogStopClock.elapsed() >= 150) {
                m_jogStopPending = false;
                m_jogStopTimer.stop();
            }
        }
        if (m_continuousJogActive &&
            (parts[0] == QLatin1String("Alarm") || parts[0] == QLatin1String("Check"))) {
            stopContinuousJog();
        }
    }

    static QRegularExpression posRe("(?:WPos|MPos):(-?[\\d\\.]+),(-?[\\d\\.]+),(-?[\\d\\.]+)");
    auto match = posRe.match(statusStr);
    if (match.hasMatch()) {
        double x = match.captured(1).toDouble();
        double y = match.captured(2).toDouble();
        double z = match.captured(3).toDouble();
        m_lastXPosition = x;
        m_lastYPosition = y;
        m_lastZPosition = z;
        m_wposXLabel->setText(QString::number(x, 'f', 3));
        m_wposYLabel->setText(QString::number(y, 'f', 3));
        m_wposZLabel->setText(QString::number(z, 'f', 3));
        emit positionUpdated(x, y, z);
    }
}

void CNCControlPanel::onSerialStatusChanged(bool connected)
{
    m_connectButton->setChecked(connected);
    m_connectButton->setText(connected ? "Disconnect" : "Connect");
    m_serialPortCombo->setEnabled(!connected);
    m_refreshButton->setEnabled(!connected);
    if (!connected) {
        m_commandQueue.clear();
        m_waitingForOk = false;
        m_pendingScanEvents.clear();
        m_lastState.clear();
        m_statusPollTimer.stop();
        stopContinuousJog();
        cancelAutofocus(QStringLiteral("Autofocus aborted: the stage link went down."));
    } else {
        m_statusPollTimer.start(m_statusPollIntervalMs);
    }

    for (auto *btn : {m_forwardButton, m_backButton, m_leftButton, m_rightButton,
                      m_upButton, m_downButton, m_homeButton, m_resetButton,
                      m_rebootButton, m_sendCommandButton}) {
        btn->setEnabled(connected);
    }
    m_commandInput->setEnabled(connected);
    m_xyFeedrateInput->setEnabled(connected);
    m_zFeedrateInput->setEnabled(connected);
    updateAutofocusControlsEnabled();
}

void CNCControlPanel::sendCommand(const QString &cmd)
{
    if (m_logWindow)
        m_logWindow->appendPlainText("[TX] " + cmd);

    enqueueCommand(cmd);
}

void CNCControlPanel::enqueueCommand(const QString &cmd)
{
    if (m_logWindow)
        m_logWindow->appendPlainText("[QUEUE] " + cmd);

    m_commandQueue.append(cmd);
    processQueue();
}

void CNCControlPanel::processQueue()
{
    if (m_logWindow && !m_commandQueue.isEmpty())
        m_logWindow->appendPlainText("[PROCESS] " + m_commandQueue.first());

    if (!m_waitingForOk && !m_commandQueue.isEmpty()) {
        QString cmd = m_commandQueue.takeFirst();

        if (cmd == "__SCAN_ROW_START__") {
            m_pendingScanEvents.append(PendingScanEvent::RowStart);
            if (flushPendingScanEventIfIdle())
                processQueue();
            return;
        }

        if (cmd == "__SCAN_ROW_END__") {
            m_pendingScanEvents.append(PendingScanEvent::RowReady);
            if (flushPendingScanEventIfIdle())
                processQueue();
            return;
        }

        if (cmd == "__SCAN_DONE__") {
            m_pendingScanEvents.append(PendingScanEvent::ScanFinished);
            if (flushPendingScanEventIfIdle())
                processQueue();
            return;
        }

        m_lastSentCommand = cmd;
        m_waitingForOk = m_serialWorker->send_command(cmd.toStdString());
        if (!m_waitingForOk) {
            m_lastSentCommand.clear();
            QTimer::singleShot(0, this, &CNCControlPanel::processQueue);
        }
    }
}

bool CNCControlPanel::flushPendingScanEventIfIdle()
{
    if (m_lastState != "Idle" || m_pendingScanEvents.isEmpty())
        return false;

    PendingScanEvent pending = m_pendingScanEvents.takeFirst();

    if (pending == PendingScanEvent::RowStart) {
        emit scanRowStartReady();
        return true;
    }

    if (pending == PendingScanEvent::RowReady) {
        emit scanRowReady();
        return true;
    }

    if (pending == PendingScanEvent::ScanFinished) {
        emit scanFinished();
        emit logSignal("Scan finished.");
        return true;
    }

    return false;
}

void CNCControlPanel::pollStatus()
{
    m_serialWorker->send_raw_command("?");
}

void CNCControlPanel::onStepSizeChanged(double v) { m_stepSize = v; }
void CNCControlPanel::onZStepSizeChanged(double v) { m_zStepSize = v; }
void CNCControlPanel::onXYFeedrateChanged(int v) { m_xyFeedrate = v; }
void CNCControlPanel::onZFeedrateChanged(int v) { m_zFeedrate = v; }

void CNCControlPanel::moveUp()
{
    sendCommand(QString("$J=G91 Z%1 F%2").arg(m_zStepSize).arg(m_zFeedrate));
}

void CNCControlPanel::moveDown()
{
    sendCommand(QString("$J=G91 Z-%1 F%2").arg(m_zStepSize).arg(m_zFeedrate));
}

void CNCControlPanel::moveLeft()
{
    sendCommand(QString("$J=G91 X-%1 F%2").arg(m_stepSize).arg(m_xyFeedrate));
}

void CNCControlPanel::moveRight()
{
    sendCommand(QString("$J=G91 X%1 F%2").arg(m_stepSize).arg(m_xyFeedrate));
}

void CNCControlPanel::moveForward()
{
    sendCommand(QString("$J=G91 Y%1 F%2").arg(m_stepSize).arg(m_xyFeedrate));
}

void CNCControlPanel::moveBack()
{
    sendCommand(QString("$J=G91 Y-%1 F%2").arg(m_stepSize).arg(m_xyFeedrate));
}

void CNCControlPanel::home()
{
    sendCommand("$H");
}

void CNCControlPanel::resetCnc()
{
    m_serialWorker->send_raw_command("\x18");
}

void CNCControlPanel::rebootCnc()
{
    m_serialWorker->send_raw_command("\x14");
    m_serialWorker->send_raw_command("\x04");
}

void CNCControlPanel::sendConsoleCommand()
{
    QString cmd = m_commandInput->text().trimmed();
    if (!cmd.isEmpty()) {
        sendCommand(cmd);
        m_commandInput->clear();
    }
}

void CNCControlPanel::setStatusPollIntervalMs(int intervalMs)
{
    m_statusPollIntervalMs = std::max(15, intervalMs);
    if (m_statusPollTimer.isActive()) {
        m_statusPollTimer.setInterval(m_statusPollIntervalMs);
    }
}

void CNCControlPanel::stop()
{
    stopContinuousJog();
    cancelAutofocus(QStringLiteral("Autofocus aborted."));
    m_serialPollTimer.stop();
    m_statusPollTimer.stop();
}

// ---------- Closed-loop autofocus ----------

void CNCControlPanel::setAutofocusAllowed(bool allowed)
{
    if (m_autofocusAllowed == allowed) {
        return;
    }
    m_autofocusAllowed = allowed;
    if (!allowed) {
        stopContinuousJog();
        cancelAutofocus(QStringLiteral("Autofocus aborted: the stage is busy with a scan."));
    }
    updateAutofocusControlsEnabled();
}

void CNCControlPanel::updateAutofocusControlsEnabled()
{
    if (!m_autofocusButton) {
        return;
    }
    const bool active = (m_focusPhase != FocusPhase::Inactive);
    // While a search runs the button stays enabled so it can act as Stop, but the
    // search parameters are locked so they cannot change mid-sweep.
    m_autofocusButton->setEnabled(active
                                  || (isStageConnected() && hasControllerStatus() && m_autofocusAllowed));
    m_autofocusButton->setText(active ? QStringLiteral("Stop") : QStringLiteral("Autofocus"));
    if (m_focusRangeInput) {
        m_focusRangeInput->setEnabled(!active);
    }
    if (m_focusStepInput) {
        m_focusStepInput->setEnabled(!active);
    }
}

void CNCControlPanel::onAutofocusClicked()
{
    if (m_focusPhase != FocusPhase::Inactive) {
        cancelAutofocus(QStringLiteral("Autofocus cancelled by user."));
        return;
    }
    startAutofocus();
}

void CNCControlPanel::submitFocusSample(double score)
{
    if (!std::isfinite(score)) {
        return;
    }
    m_lastFocusScore = score;
    if (m_focusScoreLabel) {
        m_focusScoreLabel->setText(QString("Focus: %1").arg(score, 0, 'f', 1));
    }
    if (m_focusPhase != FocusPhase::Sample) {
        return;
    }
    m_focusScoreSum += score;
    ++m_focusScoreCount;
}

bool CNCControlPanel::startAutofocus(double searchRangeMm, double coarseStepMm)
{
    if (m_focusPhase != FocusPhase::Inactive) {
        emit logSignal(QStringLiteral("Autofocus is already running."));
        return false;
    }
    stopContinuousJog();
    if (!isStageConnected()) {
        emit logSignal(QStringLiteral("Autofocus refused: the stage is not connected."));
        return false;
    }
    if (!hasControllerStatus()) {
        emit logSignal(QStringLiteral("Autofocus refused: the stage controller has not reported a status."));
        return false;
    }
    if (!std::isfinite(m_lastZPosition)) {
        emit logSignal(QStringLiteral("Autofocus refused: the stage has not reported a Z position yet."));
        return false;
    }

    const double range = searchRangeMm > 0.0
                             ? searchRangeMm
                             : (m_focusRangeInput ? m_focusRangeInput->value() : 1.0);
    const double coarse = coarseStepMm > 0.0
                             ? coarseStepMm
                             : (m_focusStepInput ? m_focusStepInput->value() : 0.05);
    if (range < 0.01 || range > 10.0) {
        emit logSignal(QStringLiteral("Autofocus refused: the search range must be between 0.01 and 10 mm."));
        return false;
    }
    if (coarse < 0.001 || coarse > range / 2.0) {
        emit logSignal(QStringLiteral(
            "Autofocus refused: the step must be at least 0.001 mm and no more than half the search range."));
        return false;
    }

    m_focusCoarseStepMm = coarse;
    m_focusFineStepMm = std::max(0.0005, coarse / kFocusFineDivisor);
    m_focusFeedrate = m_zFeedrateInput ? m_zFeedrateInput->value() : m_zFeedrate;
    m_focusBestZ = m_lastZPosition;
    m_focusBestScore = 0.0;

    startFocusPass(0, m_lastZPosition, range / 2.0, m_focusCoarseStepMm);
    m_focusTimer.start(kFocusTickMs);
    updateAutofocusControlsEnabled();
    emit autofocusStarted();
    emit logSignal(QString("Autofocus: sweeping %1 mm around Z=%2 mm in %3 mm steps at F%4.")
                       .arg(range, 0, 'f', 3)
                       .arg(m_lastZPosition, 0, 'f', 3)
                       .arg(coarse, 0, 'f', 3)
                       .arg(m_focusFeedrate));
    return true;
}

void CNCControlPanel::startFocusPass(int pass, double centreMm, double halfWidthMm, double stepMm)
{
    m_focusPass = pass;
    m_focusTargets.clear();
    m_focusScores.clear();

    if (halfWidthMm <= 0.0 || stepMm <= 0.0) {
        m_focusTargets.append(centreMm);
        m_focusScores.append(0.0);
    } else {
        const int steps = static_cast<int>(std::floor(halfWidthMm * 2.0 / stepMm)) + 1;
        for (int i = 0; i < steps; ++i) {
            m_focusTargets.append(centreMm - halfWidthMm + i * stepMm);
            m_focusScores.append(0.0);
        }
    }
    m_focusIndex = 0;
    beginFocusMove();
}

void CNCControlPanel::beginFocusMove()
{
    const double target = m_focusTargets.at(m_focusIndex);
    m_focusTargetZ = target;
    const double delta = target - m_lastZPosition;

    if (std::abs(delta) < kFocusTolerance) {
        beginFocusSampleWindow();
        return;
    }

    m_focusPhase = FocusPhase::Move;
    m_focusClock.restart();
    // Commanded travel time plus a settle margin.  The controller is only polled
    // every 100 ms, so a short jog can be over before a status report ever shows
    // it running; waiting out the computed travel is more reliable than trying to
    // catch the transient Run state.
    const double travelMs = 60000.0 * std::abs(delta) / std::max(1, m_focusFeedrate);
    m_focusMoveWaitMs = std::max<double>(kFocusSettleMs, travelMs + kFocusSettleMs);
    sendCommand(QString("$J=G91 Z%1 F%2").arg(delta, 0, 'f', 4).arg(m_focusFeedrate));
}

void CNCControlPanel::advanceAutofocus()
{
    if (m_focusPhase == FocusPhase::Inactive) {
        return;
    }

    const qint64 elapsed = m_focusClock.elapsed();
    if (elapsed > kFocusMoveTimeoutMs) {
        cancelAutofocus(QString("Autofocus timed out waiting for the stage to reach Z=%1 mm.")
                            .arg(m_focusTargetZ, 0, 'f', 3));
        return;
    }

    if (m_focusPhase == FocusPhase::Move) {
        if (elapsed >= m_focusMoveWaitMs && m_lastState == QLatin1String("Idle")) {
            beginFocusSampleWindow();
        }
        return;
    }

    // FocusPhase::Sample
    if (elapsed >= kFocusSampleMs) {
        finishFocusSample();
    }
}

void CNCControlPanel::beginFocusSampleWindow()
{
    m_focusPhase = FocusPhase::Sample;
    m_focusScoreSum = 0.0;
    m_focusScoreCount = 0;
    m_focusClock.restart();
}

void CNCControlPanel::finishFocusSample()
{
    if (m_focusScoreCount == 0) {
        cancelAutofocus(QStringLiteral(
            "Autofocus aborted: no camera frames arrived while measuring. Start the camera first."));
        return;
    }

    m_focusScores[m_focusIndex] = m_focusScoreSum / m_focusScoreCount;
    ++m_focusIndex;

    if (m_focusIndex < m_focusTargets.size()) {
        beginFocusMove();
        return;
    }

    // This pass is done.  Pick the best point and decide what to search next.
    int best = 0;
    for (int i = 1; i < m_focusScores.size(); ++i) {
        if (m_focusScores.at(i) > m_focusScores.at(best)) {
            best = i;
        }
    }
    const double bestZ = m_focusTargets.at(best);
    const double bestScore = m_focusScores.at(best);

    switch (m_focusPass) {
    case 0:
        emit logSignal(QString("Autofocus: coarse sweep peaked at Z=%1 mm (score %2), refining.")
                           .arg(bestZ, 0, 'f', 3)
                           .arg(bestScore, 0, 'f', 1));
        startFocusPass(1, bestZ, m_focusCoarseStepMm, m_focusFineStepMm);
        return;

    case 1: {
        m_focusBestZ = bestZ;
        m_focusBestScore = bestScore;
        // Fit a parabola through the three points around the maximum so the final
        // stop can land between two measurement points.
        double vertex = bestZ;
        if (best > 0 && best + 1 < m_focusScores.size()) {
            const double y0 = m_focusScores.at(best - 1);
            const double y1 = m_focusScores.at(best);
            const double y2 = m_focusScores.at(best + 1);
            const double curvature = y0 - 2.0 * y1 + y2;
            if (curvature < -1e-9) {
                const double offset = 0.5 * (y0 - y2) / curvature;
                if (std::isfinite(offset) && std::abs(offset) <= 1.0) {
                    vertex = bestZ + offset * m_focusFineStepMm;
                }
            }
        }
        startFocusPass(2, vertex, 0.0, 0.0);
        return;
    }

    case 2:
        // The parabola vertex is only trusted if it actually measured well; noise
        // or an asymmetric peak can push it somewhere worse.
        if (bestScore >= m_focusBestScore * 0.95) {
            m_focusBestZ = bestZ;
            m_focusBestScore = bestScore;
            finishAutofocus(true, QString("Autofocus complete: Z=%1 mm, focus score %2.")
                                      .arg(bestZ, 0, 'f', 3)
                                      .arg(bestScore, 0, 'f', 1));
        } else {
            emit logSignal(QString("Autofocus: vertex Z=%1 mm measured worse than Z=%2 mm, returning.")
                               .arg(bestZ, 0, 'f', 3)
                               .arg(m_focusBestZ, 0, 'f', 3));
            startFocusPass(3, m_focusBestZ, 0.0, 0.0);
        }
        return;

    default:
        finishAutofocus(true, QString("Autofocus complete: Z=%1 mm, focus score %2.")
                                  .arg(m_focusBestZ, 0, 'f', 3)
                                  .arg(m_focusBestScore, 0, 'f', 1));
        return;
    }
}

void CNCControlPanel::finishAutofocus(bool success, const QString &message)
{
    m_focusPhase = FocusPhase::Inactive;
    m_focusTimer.stop();
    m_focusTargets.clear();
    m_focusScores.clear();
    m_focusIndex = 0;
    updateAutofocusControlsEnabled();
    emit logSignal(message);
    emit autofocusFinished(success, m_focusBestZ, m_focusBestScore, message);
}

void CNCControlPanel::cancelAutofocus(const QString &reason)
{
    if (m_focusPhase == FocusPhase::Inactive) {
        return;
    }
    // A jog may still be in flight.  Feed hold goes straight to the controller
    // instead of queueing behind the command that is already running.
    if (m_focusPhase == FocusPhase::Move && m_serialWorker) {
        m_serialWorker->send_raw_command("!");
    }
    m_commandQueue.clear();
    m_waitingForOk = false;
    m_focusBestZ = std::isfinite(m_lastZPosition) ? m_lastZPosition : m_focusBestZ;
    m_focusBestScore = m_lastFocusScore;
    finishAutofocus(false, reason.isEmpty() ? QStringLiteral("Autofocus cancelled.") : reason);
}

// ---------- Smooth continuous Hold-to-Jog ----------

void CNCControlPanel::setStageBoundsMm(double widthMm, double heightMm)
{
    m_stageWidthMm = widthMm > 0.0 ? widthMm : 0.0;
    m_stageHeightMm = heightMm > 0.0 ? heightMm : 0.0;
}

bool CNCControlPanel::isContinuousJogEnabled() const
{
    return !m_holdToJogCheck || m_holdToJogCheck->isChecked();
}

double CNCControlPanel::computeJogSpeedMultiplier(bool fineModifier) const
{
    double preset = 1.0;
    if (m_jogSpeedCombo && m_jogSpeedCombo->currentData().isValid()) {
        preset = m_jogSpeedCombo->currentData().toDouble();
    }
    if (fineModifier) {
        // Holding Shift drops speed to 0.2x (or 0.1x if already in Fine preset).
        return (preset <= 0.25) ? 0.1 : (preset * 0.2);
    }
    return preset;
}

void CNCControlPanel::sendJogCancel()
{
    // Strip any queued jog moves so they cannot fire after cancel.
    for (int i = m_commandQueue.size() - 1; i >= 0; --i) {
        if (m_commandQueue.at(i).startsWith(QLatin1String("$J="))) {
            m_commandQueue.removeAt(i);
        }
    }
    if (m_serialWorker && isStageConnected()) {
        // Real-time GRBL / FluidNC Jog Cancel (0x85): smoothly decelerates to
        // zero at configured acceleration and flushes the jog queue without alarm.
        m_serialWorker->send_raw_command("\x85");
    }
}

void CNCControlPanel::setContinuousJogVector(int dx, int dy, int dz, bool fineModifier)
{
    dx = (dx > 0) ? 1 : (dx < 0 ? -1 : 0);
    dy = (dy > 0) ? 1 : (dy < 0 ? -1 : 0);
    dz = (dz > 0) ? 1 : (dz < 0 ? -1 : 0);

    if (!isContinuousJogEnabled() || (dx == 0 && dy == 0 && dz == 0)) {
        stopContinuousJog();
        return;
    }

    if (!isStageConnected() || !hasControllerStatus() ||
        m_focusPhase != FocusPhase::Inactive || !m_autofocusAllowed) {
        stopContinuousJog();
        return;
    }

    if (dx == m_jogDx && dy == m_jogDy && dz == m_jogDz &&
        fineModifier == m_jogFineModifier &&
        (m_continuousJogActive || m_jogDispatchTimer.isActive())) {
        return;
    }

    const bool hadActiveJog = m_continuousJogActive;
    m_jogStopPending = false;
    m_jogStopTimer.stop();
    m_jogDx = dx;
    m_jogDy = dy;
    m_jogDz = dz;
    m_jogFineModifier = fineModifier;

    // Pure Z press (PageUp/PageDown/E/Q) starts with a single exact fine step
    // (m_zStepSize, scaled by Shift/Fine) so quick taps nudge focus cleanly by
    // 10 um instead of blowing past the focal plane; holding > 220 ms transitions
    // into a slow, controllable continuous Z focus sweep.
    const bool pureZ = (dx == 0 && dy == 0 && dz != 0);
    if (pureZ && !hadActiveJog) {
        m_jogDispatchTimer.stop();
        m_jogContinueTimer.stop();
        const double mult = computeJogSpeedMultiplier(fineModifier);
        const double stepScale = (mult < 1.0) ? mult : 1.0;
        const double stepMm = std::max(0.001, m_zStepSize * stepScale);
        sendCommand(QString("$J=G91 Z%1 F%2")
                        .arg(dz * stepMm, 0, 'f', 4)
                        .arg(m_zFeedrate));
        m_zHoldPending = true;
        m_jogDispatchTimer.start(220);
        return;
    }

    m_zHoldPending = false;
    if (hadActiveJog) {
        // Smoothly cancel the previous jog vector and wait briefly so FluidNC
        // processes 0x85 before accepting the new $J= command.
        sendJogCancel();
        m_continuousJogActive = false;
        m_jogSawRunning = false;
        m_jogContinueTimer.stop();
        m_jogDispatchTimer.start(35);
    } else {
        m_jogDispatchTimer.stop();
        dispatchContinuousJog();
    }
}

void CNCControlPanel::dispatchContinuousJog()
{
    m_zHoldPending = false;
    if ((m_jogDx == 0 && m_jogDy == 0 && m_jogDz == 0) ||
        !isStageConnected() || !hasControllerStatus() ||
        m_focusPhase != FocusPhase::Inactive || !m_autofocusAllowed) {
        m_continuousJogActive = false;
        m_jogSawRunning = false;
        m_jogContinueTimer.stop();
        return;
    }

    const double mult = computeJogSpeedMultiplier(m_jogFineModifier);
    const int xyFeed = std::clamp(static_cast<int>(std::lround(m_xyFeedrate * mult)), 5, 1000);
    // Manual continuous Z focus needs to stay slow relative to microscope depth of field
    // (~15 mm/min Normal, ~3 mm/min Fine/Shift, ~30 mm/min Fast when Z F=100).
    const int zFeed = std::clamp(static_cast<int>(std::lround(m_zFeedrate * 0.15 * mult)), 2, 60);

    // Bound each continuous jog chunk to ~1.5 s of travel (capped at 10 mm XY / 0.5 mm Z)
    // so even a dropped link stops quickly instead of running away across the stage.
    const double maxXyChunkMm = std::clamp((xyFeed / 60.0) * 1.5, 0.5, 10.0);
    const double maxZChunkMm = std::clamp((zFeed / 60.0) * 1.5, 0.05, 0.5);
    constexpr double kSoftLimitMarginMm = 0.5;
    constexpr double kMinTravelMm = 0.005;

    auto computeAxisTravel = [](int dir, double currentPos, double boundMax, double maxChunk) -> double {
        if (dir == 0) {
            return 0.0;
        }
        double avail = maxChunk;
        if (boundMax > 2.0 * kSoftLimitMarginMm &&
            std::isfinite(currentPos) &&
            currentPos >= -1.0 && currentPos <= boundMax + 1.0) {
            if (dir > 0) {
                avail = std::max(0.0, (boundMax - kSoftLimitMarginMm) - currentPos);
            } else {
                avail = std::max(0.0, currentPos - kSoftLimitMarginMm);
            }
        }
        return std::min(maxChunk, avail);
    };

    double travelX = computeAxisTravel(m_jogDx, m_lastXPosition, m_stageWidthMm, maxXyChunkMm);
    double travelY = computeAxisTravel(m_jogDy, m_lastYPosition, m_stageHeightMm, maxXyChunkMm);
    double travelZ = (m_jogDz != 0) ? maxZChunkMm : 0.0;

    // Keep diagonal jogs at a true 45-degree angle unless one axis is already at its soft limit.
    if (m_jogDx != 0 && m_jogDy != 0) {
        if (travelX >= kMinTravelMm && travelY >= kMinTravelMm) {
            const double d = std::min(travelX, travelY);
            travelX = d;
            travelY = d;
        }
    }

    QStringList axisTokens;
    if (m_jogDx != 0 && travelX >= kMinTravelMm) {
        axisTokens.append(QString("X%1").arg(m_jogDx * travelX, 0, 'f', 3));
    }
    if (m_jogDy != 0 && travelY >= kMinTravelMm) {
        axisTokens.append(QString("Y%1").arg(m_jogDy * travelY, 0, 'f', 3));
    }
    if (m_jogDz != 0 && travelZ >= kMinTravelMm) {
        axisTokens.append(QString("Z%1").arg(m_jogDz * travelZ, 0, 'f', 3));
    }

    if (axisTokens.isEmpty()) {
        // Already at the soft-limit boundary for all held axes.
        m_continuousJogActive = false;
        m_jogSawRunning = false;
        m_jogContinueTimer.stop();
        return;
    }

    const bool hasXy = (m_jogDx != 0 && travelX >= kMinTravelMm) || (m_jogDy != 0 && travelY >= kMinTravelMm);
    const bool hasZ = (m_jogDz != 0 && travelZ >= kMinTravelMm);
    int feed = xyFeed;
    if (hasZ && hasXy) {
        feed = std::min(xyFeed, zFeed);
    } else if (hasZ) {
        feed = zFeed;
    }

    const double chunkDistMm = std::sqrt(travelX * travelX + travelY * travelY + travelZ * travelZ);
    const double expectedMs = (feed > 0) ? (60000.0 * chunkDistMm / feed) : 500.0;
    m_jogMinChunkMs = std::max<qint64>(350, static_cast<qint64>(expectedMs * 0.7));
    m_jogSawRunning = false;
    m_jogChunkClock.restart();

    const QString cmd = QString("$J=G91 %1 F%2").arg(axisTokens.join(QLatin1Char(' '))).arg(feed);
    sendCommand(cmd);
    m_continuousJogActive = true;
    m_jogContinueTimer.start(150);
}

void CNCControlPanel::checkContinuousJogContinuation()
{
    if (!m_continuousJogActive || (m_jogDx == 0 && m_jogDy == 0 && m_jogDz == 0)) {
        m_jogContinueTimer.stop();
        return;
    }
    // Wait until the controller actually ran the previous chunk (or enough time
    // has passed) before queuing another chunk, so a stale Idle status right
    // after dispatch never double-queues a second $J=.
    const bool chunkFinished = m_jogSawRunning
        ? (m_lastState == QLatin1String("Idle"))
        : (m_lastState == QLatin1String("Idle") && m_jogChunkClock.elapsed() >= m_jogMinChunkMs);
    if (chunkFinished && !m_waitingForOk && m_commandQueue.isEmpty()) {
        dispatchContinuousJog();
    }
}

void CNCControlPanel::retryPendingJogStop()
{
    if (!m_jogStopPending || !isStageConnected()) {
        m_jogStopPending = false;
        m_jogStopTimer.stop();
        return;
    }

    const qint64 elapsed = m_jogStopClock.elapsed();
    if (elapsed > 1500) {
        m_jogStopPending = false;
        m_jogStopTimer.stop();
        return;
    }

    // Re-send 0x85 while the controller is still reporting Jog, or during the
    // first 200 ms after release to eliminate the $J= vs 0x85 ISR race.
    if (m_lastState == QLatin1String("Jog") || elapsed < 200) {
        sendJogCancel();
    }
    pollStatus();
}

void CNCControlPanel::stopContinuousJog()
{
    m_jogDispatchTimer.stop();
    m_jogContinueTimer.stop();
    // Only send 0x85 Jog Cancel if a continuous sweep was actually active; a quick
    // pure-Z tap (m_zHoldPending) should finish its exact fine Z step cleanly.
    const bool needsCancel = m_continuousJogActive;
    m_jogDx = 0;
    m_jogDy = 0;
    m_jogDz = 0;
    m_jogFineModifier = false;
    m_continuousJogActive = false;
    m_zHoldPending = false;
    m_jogSawRunning = false;
    if (needsCancel) {
        m_jogStopPending = true;
        m_jogStopClock.restart();
        sendJogCancel();
        if (isStageConnected()) {
            pollStatus();
            m_jogStopTimer.start(50);
        }
    }
}

void CNCControlPanel::sendRelativeXyJog(double dxMm, double dyMm, int feedrate)
{
    m_jogDispatchTimer.stop();
    m_jogContinueTimer.stop();
    m_jogStopTimer.stop();
    const bool hadActiveContinuous = m_continuousJogActive;
    m_jogDx = 0;
    m_jogDy = 0;
    m_jogDz = 0;
    m_jogFineModifier = false;
    m_continuousJogActive = false;
    m_zHoldPending = false;
    m_jogSawRunning = false;
    m_jogStopPending = false;

    if (hadActiveContinuous) {
        sendJogCancel();
    }

    const int f = (feedrate > 0) ? feedrate : m_xyFeedrate;
    const QString cmd = QString("$J=G91 X%1 Y%2 F%3")
                            .arg(dxMm, 0, 'f', 4)
                            .arg(dyMm, 0, 'f', 4)
                            .arg(f);
    if (hadActiveContinuous) {
        QTimer::singleShot(35, this, [this, cmd]() { sendCommand(cmd); });
    } else {
        sendCommand(cmd);
    }
}
