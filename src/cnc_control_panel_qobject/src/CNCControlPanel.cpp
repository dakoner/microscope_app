#include "CNCControlPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QSerialPortInfo>
#include <QRegularExpression>

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
}

void CNCControlPanel::refreshSerialPorts()
{
    m_serialPortCombo->clear();
    const auto ports = QSerialPortInfo::availablePorts();
    for (const auto &port : ports)
        m_serialPortCombo->addItem(port.portName());
}

void CNCControlPanel::onConnectToggled(bool checked)
{
    if (checked) {
        QString port = m_serialPortCombo->currentText();
        if (!port.isEmpty())
            m_serialWorker->connect_serial(port.toStdString(), 115200);
        else
            m_connectButton->setChecked(false);
    } else {
        m_serialWorker->disconnect_serial();
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
    }

    static QRegularExpression posRe("(?:WPos|MPos):(-?[\\d\\.]+),(-?[\\d\\.]+),(-?[\\d\\.]+)");
    auto match = posRe.match(statusStr);
    if (match.hasMatch()) {
        double x = match.captured(1).toDouble();
        double y = match.captured(2).toDouble();
        double z = match.captured(3).toDouble();
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
        m_pendingScanEvent = PendingScanEvent::None;
        m_lastState.clear();
        m_statusPollTimer.stop();
    } else {
        m_statusPollTimer.start(100);
    }

    for (auto *btn : {m_forwardButton, m_backButton, m_leftButton, m_rightButton,
                      m_upButton, m_downButton, m_homeButton, m_resetButton,
                      m_rebootButton, m_sendCommandButton}) {
        btn->setEnabled(connected);
    }
    m_commandInput->setEnabled(connected);
    m_xyFeedrateInput->setEnabled(connected);
    m_zFeedrateInput->setEnabled(connected);
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
            m_pendingScanEvent = PendingScanEvent::RowStart;
            if (flushPendingScanEventIfIdle()) {
                processQueue();
                return;
            }
            processQueue();
            return;
        }

        if (cmd == "__SCAN_ROW_END__") {
            m_pendingScanEvent = PendingScanEvent::RowReady;
            if (flushPendingScanEventIfIdle()) {
                processQueue();
                return;
            }
            processQueue();
            return;
        }

        if (cmd == "__SCAN_DONE__") {
            m_pendingScanEvent = PendingScanEvent::ScanFinished;
            if (flushPendingScanEventIfIdle()) {
                processQueue();
                return;
            }
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
    if (m_lastState != "Idle" || m_pendingScanEvent == PendingScanEvent::None)
        return false;

    PendingScanEvent pending = m_pendingScanEvent;
    m_pendingScanEvent = PendingScanEvent::None;

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

void CNCControlPanel::stop()
{
    m_serialPollTimer.stop();
    m_statusPollTimer.stop();
}
