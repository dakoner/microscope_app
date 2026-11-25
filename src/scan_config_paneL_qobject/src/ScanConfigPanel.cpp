#include "ScanConfigPanel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QButtonGroup>

ScanConfigPanel::ScanConfigPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    m_scanAreaLabel = new QLabel("Scan Area: (Shift+drag on mosaic to select; drag/edges to edit)");
    m_scanAreaLabel->setWordWrap(true);
    layout->addWidget(m_scanAreaLabel);

    // Scan style
    auto *styleLayout = new QHBoxLayout;
    m_radioLeftRight = new QRadioButton("Left-Right");
    m_radioSerpentine = new QRadioButton("Serpentine");
    m_radioSerpentine->setChecked(true);
    auto *styleGroup = new QButtonGroup(this);
    styleGroup->addButton(m_radioLeftRight);
    styleGroup->addButton(m_radioSerpentine);
    styleLayout->addWidget(m_radioLeftRight);
    styleLayout->addWidget(m_radioSerpentine);
    layout->addLayout(styleLayout);

    // Homing
    m_homeXCheckbox = new QCheckBox("Home X before each row");
    m_homeYCheckbox = new QCheckBox("Home Y before each row");
    m_homeXCheckbox->setChecked(true);
    layout->addWidget(m_homeXCheckbox);
    layout->addWidget(m_homeYCheckbox);

    // Feedrate
    auto *feedLayout = new QHBoxLayout;
    feedLayout->addWidget(new QLabel("Scan Feedrate:"));
    m_scanFeedrateInput = new QSpinBox;
    m_scanFeedrateInput->setRange(1, 1000);
    m_scanFeedrateInput->setValue(100);
    feedLayout->addWidget(m_scanFeedrateInput);
    layout->addLayout(feedLayout);

    // Status
    m_statusLabel = new QLabel("Status: Idle");
    layout->addWidget(m_statusLabel);

    m_progressBar = new QProgressBar;
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    layout->addWidget(m_progressBar);

    // Buttons
    auto *btnLayout = new QHBoxLayout;
    m_startButton = new QPushButton("Start Scan");
    m_cancelButton = new QPushButton("Cancel Scan");
    btnLayout->addWidget(m_startButton);
    btnLayout->addWidget(m_cancelButton);
    layout->addLayout(btnLayout);

    m_cancelButton->setEnabled(false);
    m_startButton->setEnabled(false);

    connect(m_startButton, &QPushButton::clicked, this, &ScanConfigPanel::onStartClicked);
    connect(m_cancelButton, &QPushButton::clicked, this, &ScanConfigPanel::cancelScanSignal);
}

void ScanConfigPanel::updateScanAreas(const QVector<QRectF> &areas)
{
    m_scanAreas = areas;
    if (areas.isEmpty()) {
        m_scanAreaLabel->setText("Scan Area: (Shift+drag on mosaic to select; drag/edges to edit)");
    } else if (areas.size() == 1) {
        const QRectF &r = areas.first();
        m_scanAreaLabel->setText(
            QString("Scan Area: X %1-%2, Y %3-%4 mm (%5 x %6 mm)")
                .arg(r.left(), 0, 'f', 2)
                .arg(r.right(), 0, 'f', 2)
                .arg(r.top(), 0, 'f', 2)
                .arg(r.bottom(), 0, 'f', 2)
                .arg(r.width(), 0, 'f', 2)
                .arg(r.height(), 0, 'f', 2));
    } else {
        m_scanAreaLabel->setText(QString("Scan Area: %1 areas selected").arg(areas.size()));
    }
    m_startButton->setEnabled(!areas.isEmpty());
    scanFinished(false);
}

void ScanConfigPanel::scanStarted()
{
    m_startButton->setEnabled(false);
    m_homeXCheckbox->setEnabled(false);
    m_homeYCheckbox->setEnabled(false);
    m_radioLeftRight->setEnabled(false);
    m_radioSerpentine->setEnabled(false);
    m_scanFeedrateInput->setEnabled(false);
    m_cancelButton->setEnabled(true);
    updateStatus("Scan started...");
}

void ScanConfigPanel::onStartClicked()
{
    scanStarted();

    emit startScanSignal(m_scanAreas,
                         m_homeXCheckbox->isChecked(),
                         m_homeYCheckbox->isChecked(),
                         m_radioSerpentine->isChecked(),
                         m_scanFeedrateInput->value());
}

void ScanConfigPanel::scanFinished(bool /* success */)
{
    m_startButton->setEnabled(!m_scanAreas.isEmpty());
    m_cancelButton->setEnabled(false);
    m_homeXCheckbox->setEnabled(true);
    m_homeYCheckbox->setEnabled(true);
    m_radioLeftRight->setEnabled(true);
    m_radioSerpentine->setEnabled(true);
    m_scanFeedrateInput->setEnabled(true);
}

void ScanConfigPanel::updateStatus(const QString &status)
{
    m_statusLabel->setText("Status: " + status);
}

void ScanConfigPanel::updateProgress(int current, int total)
{
    m_progressBar->setMaximum(total);
    m_progressBar->setValue(current);
}
