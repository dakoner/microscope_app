#include "ScanConfigPanel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QButtonGroup>

ScanConfigPanel::ScanConfigPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_scanAreaLabel = new QLabel("Scan Area: (select area on mosaic)");
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
    m_scanAreaLabel->setText(QString("Scan Area: %1 area(s) selected").arg(areas.size()));
    m_startButton->setEnabled(!areas.isEmpty());
    scanFinished(false);
}

void ScanConfigPanel::onStartClicked()
{
    m_startButton->setEnabled(false);
    m_homeXCheckbox->setEnabled(false);
    m_homeYCheckbox->setEnabled(false);
    m_radioLeftRight->setEnabled(false);
    m_radioSerpentine->setEnabled(false);
    m_scanFeedrateInput->setEnabled(false);
    m_cancelButton->setEnabled(true);

    emit startScanSignal(m_scanAreas,
                         m_homeXCheckbox->isChecked(),
                         m_homeYCheckbox->isChecked(),
                         m_radioSerpentine->isChecked(),
                         m_scanFeedrateInput->value());
    updateStatus("Scan started...");
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
