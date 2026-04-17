#ifndef LEDCONTROLLER_H
#define LEDCONTROLLER_H

#include <QObject>
#include <QTimer>
#include <QMap>
#include <QPushButton>
#include <QComboBox>
#include <QSpinBox>
#include <QListWidget>
#include <QTabWidget>
#include <QLineEdit>

#include "SerialWorker.h"

class LEDController : public QObject
{
    Q_OBJECT
public:
    explicit LEDController(QWidget *parent = nullptr);
    ~LEDController() override;

    QWidget *widget() const { return m_widget; }
    void stop();
    void setPort(const QString &portName);
    QString getPort() const;

signals:
    void logSignal(const QString &msg);

private slots:
    void refreshSerialPorts();
    void onConnectClicked(bool checked);
    void onSerialLog(const std::string &msg);
    void onSerialStatusChanged(bool connected);
    void processSerialLine(const QString &line);

    void onCmdPulse();
    void onCmdPwm();
    void onCmdStopPwm();
    void onCmdRepeat();
    void onCmdStopRepeat();
    void onCmdInterrupt();
    void onCmdStopInterrupt();
    void onCmdThrob();
    void onCmdStopThrob();
    void onCmdMem();
    void togglePinLevel(int pin);

private:
    void buildUi();
    void updatePinStatus(int pin, const QString &status);
    void updateInterruptStatus(int pin, const QString &status);

    QWidget *m_widget = nullptr;
    SerialWorker *m_serialWorker = nullptr;
    QTimer m_serialPollTimer;
    bool m_hasInitialized = false;

    // UI elements
    QComboBox *m_serialPortCombo = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QTabWidget *m_cmdTabs = nullptr;
    QListWidget *m_statusList = nullptr;
    QListWidget *m_interruptList = nullptr;

    // Pulse
    QSpinBox *m_spinPulsePin = nullptr;
    QSpinBox *m_spinPulseVal = nullptr;
    QSpinBox *m_spinPulseDur = nullptr;

    // PWM
    QSpinBox *m_spinPwmPin = nullptr;
    QSpinBox *m_spinPwmFreq = nullptr;
    QSpinBox *m_spinPwmDuty = nullptr;

    // Repeat
    QSpinBox *m_spinRepeatPin = nullptr;
    QSpinBox *m_spinRepeatFreq = nullptr;
    QSpinBox *m_spinRepeatDur = nullptr;

    // Interrupt
    QSpinBox *m_spinIntPin = nullptr;
    QComboBox *m_comboIntEdge = nullptr;
    QSpinBox *m_spinIntTarget = nullptr;
    QSpinBox *m_spinIntWidth = nullptr;

    // Throb
    QSpinBox *m_spinThrobPeriod = nullptr;
    QSpinBox *m_spinThrobP1 = nullptr;
    QSpinBox *m_spinThrobP2 = nullptr;
    QSpinBox *m_spinThrobP3 = nullptr;

    // Mem
    QLineEdit *m_editMemAddr = nullptr;

    QMap<int, QListWidgetItem *> m_statusItemsMap;
    QMap<int, QListWidgetItem *> m_interruptItemsMap;
    QMap<int, QPushButton *> m_pinButtons;
};

#endif // LEDCONTROLLER_H
