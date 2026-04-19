#ifndef SCANCONFIGPANEL_H
#define SCANCONFIGPANEL_H

#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QCheckBox>
#include <QRadioButton>
#include <QSpinBox>

class ScanConfigPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ScanConfigPanel(QWidget *parent = nullptr);

    void updateScanAreas(const QVector<QRectF> &areas);
    void scanFinished(bool success);

public slots:
    void updateStatus(const QString &status);
    void updateProgress(int current, int total);

signals:
    // areas, homeX, homeY, isSerpentine, scanFeedrate
    void startScanSignal(const QVector<QRectF> &areas, bool homeX, bool homeY,
                         bool serpentine, int feedrate);
    void cancelScanSignal();

private slots:
    void onStartClicked();

private:
    QLabel *m_scanAreaLabel = nullptr;
    QRadioButton *m_radioSerpentine = nullptr;
    QRadioButton *m_radioLeftRight = nullptr;
    QCheckBox *m_homeXCheckbox = nullptr;
    QCheckBox *m_homeYCheckbox = nullptr;
    QSpinBox *m_scanFeedrateInput = nullptr;
    QLabel *m_statusLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_cancelButton = nullptr;

    QVector<QRectF> m_scanAreas;
};

#endif // SCANCONFIGPANEL_H
