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
    }

    static QRegularExpression posRe("(?:WPos|MPos):(-?[\\d\\.]+),(-?[\\d\\.]+),(-?[\\d\\.]+)");
    auto match = posRe.match(statusStr);
    if (match.hasMatch()) {
        double x = match.captured(1).toDouble();
        double y = match.captured(2).toDouble();
        double z = match.captured(3).toDouble();
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
