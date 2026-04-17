#include "LEDController.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QGroupBox>
#include <QSerialPortInfo>

LEDController::LEDController(QWidget *parent)
    : QObject(parent)
{
    m_serialWorker = new SerialWorker(this);
    buildUi();

    m_serialWorker->register_log_callback([this](std::string msg) {
        QMetaObject::invokeMethod(this, [this, msg = QString::fromStdString(msg)]() {
            onSerialLog(msg.toStdString());
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

    refreshSerialPorts();
}

LEDController::~LEDController()
{
    stop();
}

void LEDController::buildUi()
{
    m_widget = new QWidget;
    auto *mainLayout = new QVBoxLayout(m_widget);

    // Connection row
    auto *connLayout = new QHBoxLayout;
    m_serialPortCombo = new QComboBox;
    m_refreshButton = new QPushButton("Refresh");
    m_connectButton = new QPushButton("Connect");
    m_connectButton->setCheckable(true);
    connLayout->addWidget(m_serialPortCombo);
    connLayout->addWidget(m_refreshButton);
    connLayout->addWidget(m_connectButton);
    mainLayout->addLayout(connLayout);

    connect(m_refreshButton, &QPushButton::clicked, this, &LEDController::refreshSerialPorts);
    connect(m_connectButton, &QPushButton::toggled, this, &LEDController::onConnectClicked);

    // Pin buttons grid (subset of valid ESP32 pins)
    auto *pinsGroup = new QGroupBox("Pin Levels");
    auto *pinsLayout = new QHBoxLayout(pinsGroup);
    const QVector<int> validPins = {4, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
    for (int pin : validPins) {
        auto *btn = new QPushButton(QString::number(pin));
        btn->setFixedWidth(40);
        connect(btn, &QPushButton::clicked, this, [this, pin]() { togglePinLevel(pin); });
        pinsLayout->addWidget(btn);
        m_pinButtons[pin] = btn;
    }
    mainLayout->addWidget(pinsGroup);

    // Command tabs
    m_cmdTabs = new QTabWidget;
    m_cmdTabs->setEnabled(false);

    // Pulse tab
    auto *pulseTab = new QWidget;
    auto *pulseLayout = new QFormLayout(pulseTab);
    m_spinPulsePin = new QSpinBox; m_spinPulsePin->setRange(0, 39);
    m_spinPulseVal = new QSpinBox; m_spinPulseVal->setRange(0, 1);
    m_spinPulseDur = new QSpinBox; m_spinPulseDur->setRange(1, 100000); m_spinPulseDur->setValue(1000);
    pulseLayout->addRow("Pin:", m_spinPulsePin);
    pulseLayout->addRow("Value:", m_spinPulseVal);
    pulseLayout->addRow("Duration (us):", m_spinPulseDur);
    auto *btnPulse = new QPushButton("Pulse");
    connect(btnPulse, &QPushButton::clicked, this, &LEDController::onCmdPulse);
    pulseLayout->addRow(btnPulse);
    m_cmdTabs->addTab(pulseTab, "Pulse");

    // PWM tab
    auto *pwmTab = new QWidget;
    auto *pwmLayout = new QFormLayout(pwmTab);
    m_spinPwmPin = new QSpinBox; m_spinPwmPin->setRange(0, 39);
    m_spinPwmFreq = new QSpinBox; m_spinPwmFreq->setRange(1, 1000000); m_spinPwmFreq->setValue(1000);
    m_spinPwmDuty = new QSpinBox; m_spinPwmDuty->setRange(0, 100); m_spinPwmDuty->setValue(50);
    pwmLayout->addRow("Pin:", m_spinPwmPin);
    pwmLayout->addRow("Frequency:", m_spinPwmFreq);
    pwmLayout->addRow("Duty %:", m_spinPwmDuty);
    auto *btnPwm = new QPushButton("Start PWM");
    auto *btnStopPwm = new QPushButton("Stop PWM");
    connect(btnPwm, &QPushButton::clicked, this, &LEDController::onCmdPwm);
    connect(btnStopPwm, &QPushButton::clicked, this, &LEDController::onCmdStopPwm);
    pwmLayout->addRow(btnPwm);
    pwmLayout->addRow(btnStopPwm);
    m_cmdTabs->addTab(pwmTab, "PWM");

    // Repeat tab
    auto *repeatTab = new QWidget;
    auto *repeatLayout = new QFormLayout(repeatTab);
    m_spinRepeatPin = new QSpinBox; m_spinRepeatPin->setRange(0, 39);
    m_spinRepeatFreq = new QSpinBox; m_spinRepeatFreq->setRange(1, 1000000); m_spinRepeatFreq->setValue(1000);
    m_spinRepeatDur = new QSpinBox; m_spinRepeatDur->setRange(1, 100000); m_spinRepeatDur->setValue(500);
    repeatLayout->addRow("Pin:", m_spinRepeatPin);
    repeatLayout->addRow("Freq:", m_spinRepeatFreq);
    repeatLayout->addRow("Duration (us):", m_spinRepeatDur);
    auto *btnRepeat = new QPushButton("Start Repeat");
    auto *btnStopRepeat = new QPushButton("Stop Repeat");
    connect(btnRepeat, &QPushButton::clicked, this, &LEDController::onCmdRepeat);
    connect(btnStopRepeat, &QPushButton::clicked, this, &LEDController::onCmdStopRepeat);
    repeatLayout->addRow(btnRepeat);
    repeatLayout->addRow(btnStopRepeat);
    m_cmdTabs->addTab(repeatTab, "Repeat");

    // Interrupt tab
    auto *intTab = new QWidget;
    auto *intLayout = new QFormLayout(intTab);
    m_spinIntPin = new QSpinBox; m_spinIntPin->setRange(0, 39);
    m_comboIntEdge = new QComboBox;
    m_comboIntEdge->addItems({"rising", "falling", "both"});
    m_spinIntTarget = new QSpinBox; m_spinIntTarget->setRange(0, 39);
    m_spinIntWidth = new QSpinBox; m_spinIntWidth->setRange(1, 100000); m_spinIntWidth->setValue(100);
    intLayout->addRow("Pin:", m_spinIntPin);
    intLayout->addRow("Edge:", m_comboIntEdge);
    intLayout->addRow("Target:", m_spinIntTarget);
    intLayout->addRow("Width (us):", m_spinIntWidth);
    auto *btnInt = new QPushButton("Set Interrupt");
    auto *btnStopInt = new QPushButton("Stop Interrupt");
    connect(btnInt, &QPushButton::clicked, this, &LEDController::onCmdInterrupt);
    connect(btnStopInt, &QPushButton::clicked, this, &LEDController::onCmdStopInterrupt);
    intLayout->addRow(btnInt);
    intLayout->addRow(btnStopInt);
    m_cmdTabs->addTab(intTab, "Interrupt");

    // Throb tab
    auto *throbTab = new QWidget;
    auto *throbLayout = new QFormLayout(throbTab);
    m_spinThrobPeriod = new QSpinBox; m_spinThrobPeriod->setRange(1, 10000); m_spinThrobPeriod->setValue(1000);
    m_spinThrobP1 = new QSpinBox; m_spinThrobP1->setRange(0, 39);
    m_spinThrobP2 = new QSpinBox; m_spinThrobP2->setRange(0, 39);
    m_spinThrobP3 = new QSpinBox; m_spinThrobP3->setRange(0, 39);
    throbLayout->addRow("Period:", m_spinThrobPeriod);
    throbLayout->addRow("Pin 1:", m_spinThrobP1);
    throbLayout->addRow("Pin 2:", m_spinThrobP2);
    throbLayout->addRow("Pin 3:", m_spinThrobP3);
    auto *btnThrob = new QPushButton("Start Throb");
    auto *btnStopThrob = new QPushButton("Stop Throb");
    connect(btnThrob, &QPushButton::clicked, this, &LEDController::onCmdThrob);
    connect(btnStopThrob, &QPushButton::clicked, this, &LEDController::onCmdStopThrob);
    throbLayout->addRow(btnThrob);
    throbLayout->addRow(btnStopThrob);
    m_cmdTabs->addTab(throbTab, "Throb");

    // Info/Mem tab
    auto *infoTab = new QWidget;
    auto *infoLayout = new QVBoxLayout(infoTab);
    auto *btnInfo = new QPushButton("Info");
    auto *btnWifi = new QPushButton("WiFi");
    connect(btnInfo, &QPushButton::clicked, this, [this]() {
        m_serialWorker->send_command("info");
    });
    connect(btnWifi, &QPushButton::clicked, this, [this]() {
        m_serialWorker->send_command("wifi");
    });
    auto *memLayout = new QHBoxLayout;
    m_editMemAddr = new QLineEdit;
    m_editMemAddr->setPlaceholderText("Address");
    auto *btnMem = new QPushButton("Read Mem");
    connect(btnMem, &QPushButton::clicked, this, &LEDController::onCmdMem);
    memLayout->addWidget(m_editMemAddr);
    memLayout->addWidget(btnMem);
    infoLayout->addWidget(btnInfo);
    infoLayout->addWidget(btnWifi);
    infoLayout->addLayout(memLayout);
    m_cmdTabs->addTab(infoTab, "Info");

    mainLayout->addWidget(m_cmdTabs);

    // Status lists
    auto *listsLayout = new QHBoxLayout;
    auto *statusGroup = new QGroupBox("Pin Status");
    auto *statusGroupLayout = new QVBoxLayout(statusGroup);
    m_statusList = new QListWidget;
    statusGroupLayout->addWidget(m_statusList);
    auto *intGroup = new QGroupBox("Interrupts");
    auto *intGroupLayout = new QVBoxLayout(intGroup);
    m_interruptList = new QListWidget;
    intGroupLayout->addWidget(m_interruptList);
    listsLayout->addWidget(statusGroup);
    listsLayout->addWidget(intGroup);
    mainLayout->addLayout(listsLayout);
}

void LEDController::stop()
{
    m_serialPollTimer.stop();
}

void LEDController::refreshSerialPorts()
{
    m_serialPortCombo->clear();
    const auto ports = QSerialPortInfo::availablePorts();
    for (const auto &port : ports)
        m_serialPortCombo->addItem(port.portName());
}

void LEDController::onConnectClicked(bool checked)
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

void LEDController::onSerialLog(const std::string &msg)
{
    QString qmsg = QString::fromStdString(msg);
    emit logSignal(qmsg);
    if (qmsg.startsWith("Rx: "))
        processSerialLine(qmsg.mid(4).trimmed());
}

void LEDController::onSerialStatusChanged(bool connected)
{
    m_connectButton->setChecked(connected);
    m_connectButton->setText(connected ? "Disconnect" : "Connect");
    m_cmdTabs->setEnabled(connected);
    m_serialPortCombo->setEnabled(!connected);
    m_refreshButton->setEnabled(!connected);
    if (!connected) {
        m_hasInitialized = false;
        m_statusList->clear();
        m_statusItemsMap.clear();
        m_interruptList->clear();
        m_interruptItemsMap.clear();
    }
}

void LEDController::processSerialLine(const QString &line)
{
    if (line.contains("LED>") && !m_hasInitialized) {
        m_hasInitialized = true;
        QTimer::singleShot(500, this, [this]() {
            m_serialWorker->send_command("printsettings");
        });
        return;
    }

    QStringList parts = line.split(' ', Qt::SkipEmptyParts);
    if (parts.isEmpty()) return;

    QString cmd = parts[0];
    bool ok = false;

    if (cmd == "level" && parts.size() >= 3) {
        int pin = parts[1].toInt(&ok);
        if (!ok) return;
        int val = parts[2].toInt();
        if (m_statusItemsMap.contains(pin)) {
            auto *item = m_statusItemsMap.take(pin);
            delete m_statusList->takeItem(m_statusList->row(item));
        }
        if (m_pinButtons.contains(pin)) {
            m_pinButtons[pin]->setStyleSheet(
                val == 1 ? "background-color: red; color: white;" : "background-color: none;");
        }
    } else if (cmd == "pwm" && parts.size() >= 4) {
        int pin = parts[1].toInt(&ok);
        if (!ok) return;
        int freq = parts[2].toInt();
        int duty = parts[3].toInt();
        updatePinStatus(pin, QString("PWM: %1Hz, %2%").arg(freq).arg(duty));
        m_spinPwmPin->setValue(pin);
        m_spinPwmFreq->setValue(freq);
        m_spinPwmDuty->setValue(duty);
    } else if (cmd == "repeat" && parts.size() >= 4) {
        int pin = parts[1].toInt(&ok);
        if (!ok) return;
        int freq = parts[2].toInt();
        int dur = parts[3].toInt();
        updatePinStatus(pin, QString("Repeat: %1Hz, %2us").arg(freq).arg(dur));
    } else if (cmd == "throb" && parts.size() >= 5) {
        int p1 = parts[2].toInt();
        int p2 = parts[3].toInt();
        int p3 = parts[4].toInt();
        updatePinStatus(p1, "Throb");
        updatePinStatus(p2, "Throb");
        updatePinStatus(p3, "Throb");
    } else if (cmd == "interrupt" && parts.size() >= 5) {
        int pin = parts[1].toInt(&ok);
        if (!ok) return;
        QString edge = parts[2];
        int tgt = parts[3].toInt();
        int width = parts[4].toInt();
        updateInterruptStatus(pin, QString("%1 -> Pulse %2 (%3us)").arg(edge).arg(tgt).arg(width));
    }
}

void LEDController::updatePinStatus(int pin, const QString &status)
{
    QString text = QString("Pin %1: %2").arg(pin).arg(status);
    if (m_statusItemsMap.contains(pin)) {
        m_statusItemsMap[pin]->setText(text);
    } else {
        auto *item = new QListWidgetItem(text);
        m_statusList->addItem(item);
        m_statusItemsMap[pin] = item;
    }
}

void LEDController::updateInterruptStatus(int pin, const QString &status)
{
    QString text = QString("Pin %1: %2").arg(pin).arg(status);
    if (m_interruptItemsMap.contains(pin)) {
        m_interruptItemsMap[pin]->setText(text);
    } else {
        auto *item = new QListWidgetItem(text);
        m_interruptList->addItem(item);
        m_interruptItemsMap[pin] = item;
    }
}

void LEDController::togglePinLevel(int pin)
{
    if (!m_pinButtons.contains(pin)) return;
    bool isHigh = m_pinButtons[pin]->styleSheet().contains("red");
    int newVal = isHigh ? 0 : 1;
    m_serialWorker->send_command(
        QString("level %1 %2").arg(pin).arg(newVal).toStdString());
}

void LEDController::onCmdPulse()
{
    m_serialWorker->send_command(
        QString("pulse %1 %2 %3")
            .arg(m_spinPulsePin->value())
            .arg(m_spinPulseVal->value())
            .arg(m_spinPulseDur->value())
            .toStdString());
}

void LEDController::onCmdPwm()
{
    m_serialWorker->send_command(
        QString("pwm %1 %2 %3")
            .arg(m_spinPwmPin->value())
            .arg(m_spinPwmFreq->value())
            .arg(m_spinPwmDuty->value())
            .toStdString());
}

void LEDController::onCmdStopPwm()
{
    int pin = m_spinPwmPin->value();
    m_serialWorker->send_command(QString("stoppwm %1").arg(pin).toStdString());
    if (m_statusItemsMap.contains(pin)) {
        auto *item = m_statusItemsMap.take(pin);
        delete m_statusList->takeItem(m_statusList->row(item));
    }
    if (m_pinButtons.contains(pin))
        m_pinButtons[pin]->setStyleSheet("background-color: none;");
}

void LEDController::onCmdRepeat()
{
    m_serialWorker->send_command(
        QString("repeat %1 %2 %3")
            .arg(m_spinRepeatPin->value())
            .arg(m_spinRepeatFreq->value())
            .arg(m_spinRepeatDur->value())
            .toStdString());
}

void LEDController::onCmdStopRepeat()
{
    int pin = m_spinRepeatPin->value();
    m_serialWorker->send_command(QString("stoprepeat %1").arg(pin).toStdString());
    if (m_statusItemsMap.contains(pin)) {
        auto *item = m_statusItemsMap.take(pin);
        delete m_statusList->takeItem(m_statusList->row(item));
    }
    if (m_pinButtons.contains(pin))
        m_pinButtons[pin]->setStyleSheet("background-color: none;");
}

void LEDController::onCmdInterrupt()
{
    m_serialWorker->send_command(
        QString("interrupt %1 %2 %3 %4")
            .arg(m_spinIntPin->value())
            .arg(m_comboIntEdge->currentText())
            .arg(m_spinIntTarget->value())
            .arg(m_spinIntWidth->value())
            .toStdString());
}

void LEDController::onCmdStopInterrupt()
{
    int pin = m_spinIntPin->value();
    m_serialWorker->send_command(QString("stopinterrupt %1").arg(pin).toStdString());
    if (m_interruptItemsMap.contains(pin)) {
        auto *item = m_interruptItemsMap.take(pin);
        delete m_interruptList->takeItem(m_interruptList->row(item));
    }
}

void LEDController::onCmdThrob()
{
    m_serialWorker->send_command(
        QString("throb %1 %2 %3 %4")
            .arg(m_spinThrobPeriod->value())
            .arg(m_spinThrobP1->value())
            .arg(m_spinThrobP2->value())
            .arg(m_spinThrobP3->value())
            .toStdString());
}

void LEDController::onCmdStopThrob()
{
    m_serialWorker->send_command("stopthrob");
}

void LEDController::onCmdMem()
{
    QString addr = m_editMemAddr->text().trimmed();
    if (!addr.isEmpty())
        m_serialWorker->send_command(QString("mem %1").arg(addr).toStdString());
}

void LEDController::setPort(const QString &portName)
{
    int idx = m_serialPortCombo->findText(portName);
    if (idx >= 0)
        m_serialPortCombo->setCurrentIndex(idx);
}

QString LEDController::getPort() const
{
    return m_serialPortCombo->currentText();
}
