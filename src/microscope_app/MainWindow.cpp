#include "MainWindow.h"
#include "ui_MainWindow.h"

#include <QCloseEvent>
#include <QKeyEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMenuBar>
#include <QStatusBar>
#include <QPainter>
#include <QPen>
#include <QColor>
#include <QLineF>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QDir>
#include <QDateTime>

#include <cmath>
#include <ctime>

#include "MindVisionCamera.h"
#include "VideoThread.h"
#include "CNCControlPanel.h"
#include "LEDController.h"
#include "MosaicPanel.h"
#include "ScanConfigPanel.h"
#include "IntensityChart.h"
#include "ColorPickerWidget.h"

// ---------- helpers ----------

static double nowSec()
{
    return static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}

// ---------- ctor / dtor ----------

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    ui = new Ui::MainWindow;
    ui->setupUi(this);

    // Assign widget pointers from .ui
    m_mainSplitter = ui->m_mainSplitter;
    m_centerTabs = ui->m_centerTabs;
    m_videoLabel = ui->m_videoLabel;
    m_mosaicTabContainer = ui->m_mosaicTabContainer;
    m_logTextEdit = ui->m_logTextEdit;
    m_controlsGroup = ui->m_controlsGroup;
    m_chkAutoExposure = ui->m_chkAutoExposure;
    m_chkRoi = ui->m_chkRoi;
    m_spinExposureTime = ui->m_spinExposureTime;
    m_sliderExposure = ui->m_sliderExposure;
    m_spinGain = ui->m_spinGain;
    m_sliderGain = ui->m_sliderGain;
    m_spinAeTarget = ui->m_spinAeTarget;
    m_sliderAeTarget = ui->m_sliderAeTarget;
    m_triggerGroup = ui->m_triggerGroup;
    m_rbContinuous = ui->m_rbContinuous;
    m_rbSoftware = ui->m_rbSoftware;
    m_rbHardware = ui->m_rbHardware;
    m_triggerBg = ui->m_triggerBg;
    m_btnSoftTrigger = ui->m_btnSoftTrigger;
    m_triggerParamsGroup = ui->m_triggerParamsGroup;
    m_spinTriggerCount = ui->m_spinTriggerCount;
    m_spinTriggerDelay = ui->m_spinTriggerDelay;
    m_spinTriggerInterval = ui->m_spinTriggerInterval;
    m_extTriggerGroup = ui->m_extTriggerGroup;
    m_comboExtMode = ui->m_comboExtMode;
    m_spinExtJitter = ui->m_spinExtJitter;
    m_comboExtShutter = ui->m_comboExtShutter;
    m_strobeGroup = ui->m_strobeGroup;
    m_comboStrobeMode = ui->m_comboStrobeMode;
    m_comboStrobePolarity = ui->m_comboStrobePolarity;
    m_spinStrobeDelay = ui->m_spinStrobeDelay;
    m_spinStrobeWidth = ui->m_spinStrobeWidth;
    m_cncControlPanel = ui->m_cncControlPanel;
    m_rightTabs = ui->m_rightTabs;
    m_spinRulerLen = ui->m_spinRulerLen;
    m_lblRulerPx = ui->m_lblRulerPx;
    m_lblRulerCalib = ui->m_lblRulerCalib;
    m_lblRulerMeas = ui->m_lblRulerMeas;
    m_btnRulerCalibrate = ui->m_btnRulerCalibrate;
    m_chkShowProfile = ui->m_chkShowProfile;
    m_intensityChart = ui->m_intensityChart;
    m_tabColorPicker = ui->m_tabColorPicker;
    m_actionStartCamera = ui->m_actionStartCamera;
    m_actionStopCamera = ui->m_actionStopCamera;
    m_actionRecord = ui->m_actionRecord;
    m_actionSnapshot = ui->m_actionSnapshot;
    m_actionHomeAndRun = ui->m_actionHomeAndRun;
    m_actionRuler = ui->m_actionRuler;
    m_actionColorPicker = ui->m_actionColorPicker;

    // Post-setup customization
    m_videoLabel->installEventFilter(this);
    m_videoLabel->setFocusPolicy(Qt::StrongFocus);
    m_mainSplitter->setSizes({400, 1000});
    m_triggerBg->setId(m_rbContinuous, 0);
    m_triggerBg->setId(m_rbSoftware, 1);
    m_triggerBg->setId(m_rbHardware, 2);
    m_intensityChart->hide();
    m_rulerTabIndex = m_rightTabs->indexOf(ui->rulerPage);
    m_colorPickerTabIndex = m_rightTabs->indexOf(m_tabColorPicker);
    m_rightTabs->setTabVisible(m_colorPickerTabIndex, false);

    // FPS label (added to status bar at runtime)
    m_fpsLabel = new QLabel("FPS: 0.0");
    statusBar()->addPermanentWidget(m_fpsLabel);

    // LED Controller (replaces ledPlaceholder in .ui)
    m_ledController = new LEDController(this);
    int ledIdx = ui->hardwareTabs->indexOf(ui->ledPlaceholder);
    ui->hardwareTabs->removeTab(ledIdx);
    ui->hardwareTabs->insertTab(ledIdx, m_ledController->widget(), "LED");

    // Create picture-in-picture labels
    m_videoPipLabel = new QLabel(m_mosaicTabContainer);
    m_videoPipLabel->setFixedSize(240, 180);
    m_videoPipLabel->setStyleSheet("border: 2px solid #888; background-color: black;");
    m_videoPipLabel->setScaledContents(true);
    m_videoPipLabel->setAlignment(Qt::AlignCenter);
    m_videoPipLabel->move(m_mosaicTabContainer->width() - 250, 10);

    m_mosaicPipLabel = new QLabel(m_centerTabs->widget(0));
    m_mosaicPipLabel->setFixedSize(240, 180);
    m_mosaicPipLabel->setStyleSheet("border: 2px solid #888; background-color: black;");
    m_mosaicPipLabel->setScaledContents(true);
    m_mosaicPipLabel->setAlignment(Qt::AlignCenter);
    m_mosaicPipLabel->move(m_centerTabs->widget(0)->width() - 250, 10);

    connectSignals();

    // Camera & video
    m_camera = new MindVisionCamera(this);

    connect(m_camera, &MindVisionCamera::fpsChanged, this, &MainWindow::updateFps);
    connect(m_camera, &MindVisionCamera::errorOccurred, this, &MainWindow::handleError);
    m_videoThread = new VideoThread(this);

    // Camera frame signal – connect to frameReady
    connect(m_camera, &MindVisionCamera::frameReady, this,
            [this](QImage image, qint64 /*ts*/) {
        // Recording
        if (m_videoThread->isRunning())
            m_videoThread->addFrame(image);

        // Throttle to ~30 FPS
        double now = nowSec();
        if (now - m_lastUiUpdateTime > 0.033) {
            m_lastUiUpdateTime = now;
            updateFrame(std::move(image));
        }
    });

    // Param poll timer
    connect(&m_paramPollTimer, &QTimer::timeout, this, &MainWindow::pollCameraParams);

    // CNC panel
    if (m_cncControlPanel) {
        connect(m_cncControlPanel, &CNCControlPanel::logSignal, this, &MainWindow::log);
        connect(m_cncControlPanel, &CNCControlPanel::positionUpdated, this, &MainWindow::onCncPositionUpdated);
        connect(m_cncControlPanel, &CNCControlPanel::stateUpdated, this, &MainWindow::onCncStateUpdated);
        connect(m_cncControlPanel, &CNCControlPanel::scanFinished, this, &MainWindow::onScanFinished);
        connect(m_cncControlPanel, &CNCControlPanel::scanRowReady, this, &MainWindow::onRowFinished);
    }

    // LED controller
    connect(m_ledController, &LEDController::logSignal, this, &MainWindow::log);

    // Initial states
    m_actionStopCamera->setEnabled(false);
    m_actionRecord->setEnabled(false);
    m_actionSnapshot->setEnabled(false);
    m_controlsGroup->setEnabled(false);
    m_triggerGroup->setEnabled(false);
    m_triggerParamsGroup->setEnabled(false);
    m_extTriggerGroup->setEnabled(false);
    m_strobeGroup->setEnabled(false);

    log("Application started.");

    QTimer::singleShot(0, this, &MainWindow::loadSettings);
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::connectSignals()
{
    // Refresh video label when switching back to the camera tab
    connect(m_centerTabs, &QTabWidget::currentChanged, this, [this](int) {
        if (m_centerTabs->currentWidget() == m_videoLabel && !m_currentImage.isNull()) {
            m_currentPixmap = QPixmap::fromImage(m_currentImage);
            refreshVideoLabel();
        }
    });

    // Actions
    connect(m_actionStartCamera, &QAction::triggered, this, &MainWindow::onStartClicked);
    connect(m_actionStopCamera, &QAction::triggered, this, &MainWindow::onStopClicked);
    connect(m_actionRecord, &QAction::triggered, this, &MainWindow::onRecordClicked);
    connect(m_actionSnapshot, &QAction::triggered, this, &MainWindow::onSnapshotClicked);
    connect(m_actionHomeAndRun, &QAction::triggered, this, &MainWindow::onHomeAndRunClicked);
    connect(m_actionRuler, &QAction::toggled, this, &MainWindow::onRulerToggled);
    connect(m_actionColorPicker, &QAction::toggled, this, &MainWindow::onColorPickerToggled);

    // Camera controls
    connect(m_chkAutoExposure, &QCheckBox::toggled, this, &MainWindow::onAutoExposureToggled);
    connect(m_chkRoi, &QCheckBox::toggled, this, &MainWindow::onRoiToggled);
    connect(m_spinExposureTime, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &MainWindow::onExposureTimeChanged);
    connect(m_sliderExposure, &QSlider::valueChanged, this, &MainWindow::onExposureSliderChanged);
    connect(m_spinGain, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onGainChanged);
    connect(m_sliderGain, &QSlider::valueChanged, this, &MainWindow::onGainSliderChanged);
    connect(m_spinAeTarget, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onAeTargetChanged);
    connect(m_sliderAeTarget, &QSlider::valueChanged, this, &MainWindow::onAeSliderChanged);
    connect(m_triggerBg, &QButtonGroup::idToggled, this, &MainWindow::onTriggerModeChanged);
    connect(m_btnSoftTrigger, &QPushButton::clicked, this, &MainWindow::onSoftTriggerClicked);
    connect(m_spinTriggerCount, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onTriggerCountChanged);
    connect(m_spinTriggerDelay, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onTriggerDelayChanged);
    connect(m_spinTriggerInterval, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onTriggerIntervalChanged);
    connect(m_comboExtMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onExtModeChanged);
    connect(m_spinExtJitter, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onExtJitterChanged);
    connect(m_comboExtShutter, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onExtShutterChanged);
    connect(m_comboStrobeMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onStrobeModeChanged);
    connect(m_comboStrobePolarity, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onStrobePolarityChanged);
    connect(m_spinStrobeDelay, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onStrobeDelayChanged);
    connect(m_spinStrobeWidth, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onStrobeWidthChanged);

    // Ruler
    connect(m_btnRulerCalibrate, &QPushButton::clicked, this, &MainWindow::calibrateRuler);
    connect(m_chkShowProfile, &QCheckBox::toggled, this, &MainWindow::onShowProfileToggled);

    // PiP repositioning on tab change
    connect(m_centerTabs, &QTabWidget::currentChanged, this, [this](int) {
        QTimer::singleShot(100, this, [this]() {
            if (m_mosaicPipLabel && m_centerTabs->widget(0)) {
                m_mosaicPipLabel->move(m_centerTabs->widget(0)->width() - 250, 10);
            }
            if (m_videoPipLabel && m_mosaicTabContainer) {
                m_videoPipLabel->move(m_mosaicTabContainer->width() - 250, 10);
            }
        });
    });

    // Install event filters for resize handling
    m_mosaicTabContainer->installEventFilter(this);
    auto *videoTabWidget = m_centerTabs->widget(0);
    if (videoTabWidget) {
        videoTabWidget->installEventFilter(this);
    }
}

// ---------- Close / Event Filter ----------

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();
    onStopClicked();

    if (m_cncControlPanel)
        m_cncControlPanel->stop();
    m_ledController->stop();

    QMainWindow::closeEvent(event);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_videoLabel) {
        if (event->type() == QEvent::Resize) {
            refreshVideoLabel();
        } else if (event->type() == QEvent::KeyPress) {
            keyPressEvent(static_cast<QKeyEvent *>(event));
            return true;
        } else if (m_rulerActive) {
            auto *me = dynamic_cast<QMouseEvent *>(event);
            if (!me) return QMainWindow::eventFilter(watched, event);
            if (event->type() == QEvent::MouseButtonPress && me->button() == Qt::LeftButton) {
                QPointF pos = getImageCoords(me->position());
                if (!pos.isNull()) {
                    m_rulerStart = pos;
                    m_rulerEnd = pos;
                    m_hasRulerStart = true;
                    m_hasRulerEnd = true;
                    refreshVideoLabel();
                }
            } else if (event->type() == QEvent::MouseMove && (me->buttons() & Qt::LeftButton)) {
                QPointF pos = getImageCoords(me->position());
                if (!pos.isNull() && m_hasRulerStart) {
                    m_rulerEnd = pos;
                    m_hasRulerEnd = true;
                    updateRulerStats();
                    refreshVideoLabel();
                }
            } else if (event->type() == QEvent::MouseButtonRelease && me->button() == Qt::LeftButton) {
                QPointF pos = getImageCoords(me->position());
                if (!pos.isNull() && m_hasRulerStart) {
                    m_rulerEnd = pos;
                    m_hasRulerEnd = true;
                    updateRulerStats();
                    refreshVideoLabel();
                }
            }
        } else if (m_colorPickerActive) {
            auto *me = dynamic_cast<QMouseEvent *>(event);
            if (me && (event->type() == QEvent::MouseMove ||
                       event->type() == QEvent::MouseButtonPress)) {
                QPointF pos = getImageCoords(me->position());
                if (!pos.isNull())
                    updateColorPicker(pos);
            }
        }
    } else if (watched == m_mosaicTabContainer && event->type() == QEvent::Resize) {
        // Reposition video PiP label when mosaic tab is resized
        if (m_videoPipLabel) {
            m_videoPipLabel->move(m_mosaicTabContainer->width() - 250, 10);
        }
    } else if (m_centerTabs && watched == m_centerTabs->widget(0) && event->type() == QEvent::Resize) {
        // Reposition mosaic PiP label when video tab is resized
        if (m_mosaicPipLabel) {
            m_mosaicPipLabel->move(m_centerTabs->widget(0)->width() - 250, 10);
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    if (m_cncControlPanel) {
        switch (event->key()) {
        case Qt::Key_Left:  m_cncControlPanel->moveLeft();    return;
        case Qt::Key_Right: m_cncControlPanel->moveRight();   return;
        case Qt::Key_Up:    m_cncControlPanel->moveForward();  return;
        case Qt::Key_Down:  m_cncControlPanel->moveBack();     return;
        case Qt::Key_PageUp:   m_cncControlPanel->moveUp();    return;
        case Qt::Key_PageDown: m_cncControlPanel->moveDown();  return;
        default: break;
        }
    }
    QMainWindow::keyPressEvent(event);
}

// ---------- Video Display ----------

void MainWindow::refreshVideoLabel()
{
    if (m_currentPixmap.isNull()) return;

    QSize labelSize = m_videoLabel->size();

    if (m_rulerActive && m_hasRulerStart) {
        QPixmap display = m_currentPixmap.copy();
        QPainter painter(&display);
        QPen pen(QColor(0, 255, 255), 2);
        painter.setPen(pen);
        QPointF end = m_hasRulerEnd ? m_rulerEnd : m_rulerStart;
        painter.drawLine(m_rulerStart, end);
        pen.setWidth(4);
        painter.setPen(pen);
        painter.drawPoint(m_rulerStart);
        painter.drawPoint(end);
        painter.end();
        m_videoLabel->setPixmap(display.scaled(labelSize, Qt::KeepAspectRatio, Qt::FastTransformation));
    } else {
        m_videoLabel->setPixmap(m_currentPixmap.scaled(labelSize, Qt::KeepAspectRatio, Qt::FastTransformation));
    }
    m_lastVideoLabelSize = labelSize;

    // Update PiP video label in mosaic tab
    if (m_videoPipLabel && !m_currentPixmap.isNull()) {
        m_videoPipLabel->setPixmap(m_currentPixmap.scaled(m_videoPipLabel->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
    }
}

QPointF MainWindow::getImageCoords(const QPointF &mousePos)
{
    if (m_currentPixmap.isNull()) return {};

    double lblW = m_videoLabel->width();
    double lblH = m_videoLabel->height();
    double imgW = m_currentPixmap.width();
    double imgH = m_currentPixmap.height();
    if (imgW == 0 || imgH == 0) return {};

    double scaleW = lblW / imgW;
    double scaleH = lblH / imgH;
    double scale = std::min(scaleW, scaleH);
    double drawnW = imgW * scale;
    double drawnH = imgH * scale;
    double offX = (lblW - drawnW) / 2.0;
    double offY = (lblH - drawnH) / 2.0;

    double mx = mousePos.x();
    double my = mousePos.y();
    if (mx < offX || mx > offX + drawnW || my < offY || my > offY + drawnH)
        return {};

    return {(mx - offX) / scale, (my - offY) / scale};
}

// ---------- Camera ----------

void MainWindow::onStartClicked()
{
    if (m_camera->open()) {
        if (m_camera->start()) {
            m_isCameraRunning = true;
            m_actionStartCamera->setEnabled(false);
            m_actionStopCamera->setEnabled(true);
            m_actionRecord->setEnabled(true);
            m_actionSnapshot->setEnabled(true);
            m_controlsGroup->setEnabled(true);
            m_triggerGroup->setEnabled(true);
            m_strobeGroup->setEnabled(true);
            m_videoLabel->setText("Starting stream...");
            applyCameraSettings();
            syncUi();
            m_paramPollTimer.start(200);
            log("Camera started.");
        }
    }
}

void MainWindow::onStopClicked()
{
    m_paramPollTimer.stop();
    m_isCameraRunning = false;
    if (m_videoThread->isRunning())
        onRecordClicked();

    m_camera->stop();
    m_camera->close();

    m_actionStartCamera->setEnabled(true);
    m_actionStopCamera->setEnabled(false);
    m_actionRecord->setEnabled(false);
    m_actionSnapshot->setEnabled(false);
    m_controlsGroup->setEnabled(false);
    m_triggerGroup->setEnabled(false);
    m_triggerParamsGroup->setEnabled(false);
    m_extTriggerGroup->setEnabled(false);
    m_strobeGroup->setEnabled(false);

    m_videoLabel->clear();
    m_videoLabel->setText("Camera Stopped");
    m_fpsLabel->setText("FPS: 0.0");
    log("Camera stopped.");
}

void MainWindow::onRecordClicked()
{
    if (!m_videoThread->isRunning()) {
        m_recordingRequested = true;
        log("Recording requested...");
    } else {
        m_videoThread->stopRecording();
        m_actionRecord->setText("Record");
        log("Recording stopped.");
    }
}

void MainWindow::onSnapshotClicked()
{
    if (!m_currentImage.isNull()) {
        QDir snapDir("snapshots");
        snapDir.mkpath(".");
        QString filename = snapDir.filePath(
            QString("snapshot_%1.png").arg(QDateTime::currentSecsSinceEpoch()));
        m_currentImage.save(filename);
        log(QString("Snapshot saved: %1").arg(QFileInfo(filename).fileName()));
    }
}

void MainWindow::onHomeAndRunClicked()
{
    if (!m_cncControlPanel) return;
    int feedrate = m_cncControlPanel->feedrate();
    m_cncControlPanel->sendCommand("$H");
    m_cncControlPanel->sendCommand("G90");
    m_cncControlPanel->sendCommand(QString("G1 X0 Y0 F%1").arg(feedrate));
    m_cncControlPanel->sendCommand(QString("G1 X100 Y0 F%1").arg(feedrate));
    m_cncControlPanel->sendCommand("G4 P0");
    log("Executing Home and Run.");
}

// ---------- Frame pipeline ----------

void MainWindow::updateFrame(QImage image)
{
    if (image.isNull()) return;

    // Recording start trigger
    if (m_recordingRequested) {
        m_recordingRequested = false;
        double recordFps = m_currentFps > 0.1 ? m_currentFps : 30.0;
        QDir videoDir("videos");
        videoDir.mkpath(".");
        QString filename = videoDir.filePath(
            QString("recording_%1.mkv").arg(QDateTime::currentSecsSinceEpoch()));
        m_videoThread->startRecording(image.width(), image.height(), recordFps, filename);
        m_actionRecord->setText("Stop Recording");
        log(QString("Recording started: %1").arg(QFileInfo(filename).fileName()));
    }

    m_currentImage = image;

    // Only update the video label when its tab is visible
    bool videoTabVisible = m_centerTabs->currentWidget() == m_videoLabel;
    if (videoTabVisible) {
        m_currentPixmap = QPixmap::fromImage(m_currentImage);
        refreshVideoLabel();
    }

    // Intensity profile
    if (videoTabVisible && m_rulerActive && m_chkShowProfile->isChecked() && m_hasRulerStart) {
        QPointF end = m_hasRulerEnd ? m_rulerEnd : m_rulerStart;
        updateIntensityProfile(m_rulerStart, end, &image);
    }

    // Mosaic update (throttle to ~5 FPS)
    if (m_mosaicPanel && m_cncState != "Home") {
        double now2 = nowSec();
        if (now2 - m_lastMosaicUpdateTime > 0.2) {
            m_lastMosaicUpdateTime = now2;
            m_mosaicPanel->updateMosaic(image, m_currentCncXMm, m_currentCncYMm);
            
            // Update PiP mosaic label in video tab
            if (m_mosaicPipLabel) {
                QPixmap mosaicPixmap = m_mosaicPanel->grab();
                if (!mosaicPixmap.isNull()) {
                    m_mosaicPipLabel->setPixmap(mosaicPixmap.scaled(m_mosaicPipLabel->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
                }
            }
        }
    }
}

void MainWindow::updateFps(double fps)
{
    m_currentFps = fps;
    m_fpsLabel->setText(QString("FPS: %1").arg(fps, 0, 'f', 1));
}

void MainWindow::handleError(QString message)
{
    log(QString("Camera Error: %1").arg(message));
    m_videoLabel->setText(QString("Error: %1").arg(message));
    m_actionStartCamera->setEnabled(true);
    m_actionStopCamera->setEnabled(false);
    m_controlsGroup->setEnabled(false);
    m_triggerGroup->setEnabled(false);
    m_triggerParamsGroup->setEnabled(false);
    m_extTriggerGroup->setEnabled(false);
    m_strobeGroup->setEnabled(false);
}

// ---------- Camera settings ----------

void MainWindow::syncUi()
{
    double minExp, maxExp;
    m_camera->getExposureTimeRange(minExp, maxExp);
    double stepExp = m_camera->getExposureTimeStep();

    m_spinExposureTime->setRange(minExp, maxExp);
    if (stepExp > 0)
        m_sliderExposure->setRange(0, static_cast<int>((maxExp - minExp) / stepExp));
    else
        m_sliderExposure->setRange(0, 10000);

    int minGain, maxGain;
    m_camera->getAnalogGainRange(minGain, maxGain);
    m_spinGain->setRange(minGain, maxGain);
    m_sliderGain->setRange(minGain, maxGain);

    bool isAuto = m_camera->getAutoExposure();
    m_chkAutoExposure->setChecked(isAuto);
    m_spinExposureTime->setEnabled(!isAuto);
    m_sliderExposure->setEnabled(!isAuto);
    m_spinAeTarget->setEnabled(isAuto);
    m_sliderAeTarget->setEnabled(isAuto);

    m_spinAeTarget->setRange(0, 255);
    m_sliderAeTarget->setRange(0, 255);
    int aeTarget = m_camera->getAeTarget();
    m_spinAeTarget->blockSignals(true);
    m_spinAeTarget->setValue(aeTarget);
    m_spinAeTarget->blockSignals(false);
    m_sliderAeTarget->blockSignals(true);
    m_sliderAeTarget->setValue(aeTarget);
    m_sliderAeTarget->blockSignals(false);

    double curExp = m_camera->getExposureTime();
    m_spinExposureTime->blockSignals(true);
    m_spinExposureTime->setValue(curExp);
    m_spinExposureTime->blockSignals(false);
    updateSliderFromTime(curExp, minExp, maxExp);

    int curGain = m_camera->getAnalogGain();
    m_spinGain->blockSignals(true);
    m_spinGain->setValue(curGain);
    m_spinGain->blockSignals(false);
    m_sliderGain->blockSignals(true);
    m_sliderGain->setValue(curGain);
    m_sliderGain->blockSignals(false);
}

void MainWindow::updateSliderFromTime(double current, double minVal, double maxVal)
{
    m_sliderExposure->blockSignals(true);
    double stepExp = m_camera->getExposureTimeStep();
    if (stepExp > 0)
        m_sliderExposure->setValue(static_cast<int>(std::round((current - minVal) / stepExp)));
    else if (maxVal > minVal)
        m_sliderExposure->setValue(static_cast<int>((current - minVal) / (maxVal - minVal) * 10000));
    m_sliderExposure->blockSignals(false);
}

void MainWindow::applyCameraSettings()
{
    m_camera->setAutoExposure(m_chkAutoExposure->isChecked());
    m_camera->setExposureTime(m_spinExposureTime->value());
    m_camera->setAnalogGain(m_spinGain->value());
}

void MainWindow::pollCameraParams()
{
    if (!m_isCameraRunning) return;

    double curExp = m_camera->getExposureTime();
    if (!m_sliderExposure->isSliderDown() && !m_spinExposureTime->hasFocus()) {
        if (std::abs(m_spinExposureTime->value() - curExp) > 1.0) {
            m_spinExposureTime->blockSignals(true);
            m_spinExposureTime->setValue(curExp);
            m_spinExposureTime->blockSignals(false);
            updateSliderFromTime(curExp, m_spinExposureTime->minimum(), m_spinExposureTime->maximum());
        }
    }

    int curGain = m_camera->getAnalogGain();
    if (!m_sliderGain->isSliderDown() && !m_spinGain->hasFocus()) {
        if (std::abs(m_spinGain->value() - curGain) > 0) {
            m_spinGain->blockSignals(true);
            m_spinGain->setValue(curGain);
            m_spinGain->blockSignals(false);
            m_sliderGain->blockSignals(true);
            m_sliderGain->setValue(curGain);
            m_sliderGain->blockSignals(false);
        }
    }
}

void MainWindow::onAutoExposureToggled(bool checked)
{
    if (!m_isCameraRunning) return;
    if (m_camera->setAutoExposure(checked)) {
        m_spinExposureTime->setEnabled(!checked);
        m_sliderExposure->setEnabled(!checked);
        m_spinAeTarget->setEnabled(checked);
        m_sliderAeTarget->setEnabled(checked);
        if (!checked) {
            double curExp = m_camera->getExposureTime();
            m_spinExposureTime->setValue(curExp);
            updateSliderFromTime(curExp, m_spinExposureTime->minimum(), m_spinExposureTime->maximum());
        }
    } else {
        m_chkAutoExposure->setChecked(!checked);
    }
}

void MainWindow::onRoiToggled(bool checked)
{
    if (!m_camera->setRoi(checked))
        m_chkRoi->setChecked(!checked);
}

void MainWindow::onExposureTimeChanged(double value)
{
    if (!m_isCameraRunning) return;
    m_camera->setExposureTime(value);
    double actual = m_camera->getExposureTime();
    m_spinExposureTime->blockSignals(true);
    m_spinExposureTime->setValue(actual);
    m_spinExposureTime->blockSignals(false);
    updateSliderFromTime(actual, m_spinExposureTime->minimum(), m_spinExposureTime->maximum());
}

void MainWindow::onExposureSliderChanged(int value)
{
    double minExp = m_spinExposureTime->minimum();
    double stepExp = m_camera->getExposureTimeStep();
    double newTime;
    if (stepExp > 0) {
        newTime = minExp + value * stepExp;
    } else {
        double maxExp = m_spinExposureTime->maximum();
        newTime = minExp + (value / 10000.0) * (maxExp - minExp);
    }
    m_spinExposureTime->setValue(newTime);
}

void MainWindow::onGainChanged(int value)
{
    if (!m_isCameraRunning) return;
    m_camera->setAnalogGain(value);
    m_sliderGain->blockSignals(true);
    m_sliderGain->setValue(value);
    m_sliderGain->blockSignals(false);
}

void MainWindow::onGainSliderChanged(int value)
{
    m_spinGain->setValue(value);
}

void MainWindow::onAeTargetChanged(int value)
{
    if (!m_isCameraRunning) return;
    m_camera->setAeTarget(value);
    m_sliderAeTarget->blockSignals(true);
    m_sliderAeTarget->setValue(value);
    m_sliderAeTarget->blockSignals(false);
}

void MainWindow::onAeSliderChanged(int value)
{
    m_spinAeTarget->setValue(value);
}

void MainWindow::onTriggerModeChanged(int id, bool checked)
{
    if (!checked) return;
    if (m_camera->setTriggerMode(id)) {
        m_btnSoftTrigger->setEnabled(id == 1);
        m_triggerParamsGroup->setEnabled(id >= 1);
        m_extTriggerGroup->setEnabled(id == 2);
    } else {
        log(QString("Failed to set trigger mode %1").arg(id));
    }
}

void MainWindow::onSoftTriggerClicked() { m_camera->triggerSoftware(); }
void MainWindow::onTriggerCountChanged(int value) { m_camera->setTriggerCount(value); }
void MainWindow::onTriggerDelayChanged(int value) { m_camera->setTriggerDelay(value); }
void MainWindow::onTriggerIntervalChanged(int value) { m_camera->setTriggerInterval(value); }
void MainWindow::onExtModeChanged(int index) { m_camera->setExternalTriggerSignalType(index); }
void MainWindow::onExtJitterChanged(int value) { m_camera->setExternalTriggerJitterTime(value); }
void MainWindow::onExtShutterChanged(int index) { m_camera->setExternalTriggerShutterMode(index); }

void MainWindow::onStrobeModeChanged(int index)
{
    m_camera->setStrobeMode(index);
    bool isManual = index == 1;
    m_comboStrobePolarity->setEnabled(isManual);
    m_spinStrobeDelay->setEnabled(isManual);
    m_spinStrobeWidth->setEnabled(isManual);
}

void MainWindow::onStrobePolarityChanged(int index) { m_camera->setStrobePolarity(index); }
void MainWindow::onStrobeDelayChanged(int value) { m_camera->setStrobeDelayTime(value); }
void MainWindow::onStrobeWidthChanged(int value) { m_camera->setStrobePulseWidth(value); }

// ---------- Ruler ----------

void MainWindow::onRulerToggled(bool checked)
{
    m_rulerActive = checked;
    m_rightTabs->setTabVisible(m_rulerTabIndex, checked);
    if (checked) {
        m_rightTabs->setCurrentIndex(m_rulerTabIndex);
        if (m_colorPickerActive)
            m_actionColorPicker->setChecked(false);
    } else {
        m_hasRulerStart = false;
        m_hasRulerEnd = false;
        refreshVideoLabel();
        m_intensityChart->hide();
        m_chkShowProfile->setChecked(false);
    }
}

void MainWindow::calibrateRuler()
{
    if (!m_hasRulerStart || !m_hasRulerEnd) return;
    QLineF line(m_rulerStart, m_rulerEnd);
    double pxDist = line.length();
    if (pxDist < 1.0) { log("Ruler: Line too short to calibrate."); return; }
    int knownMm = m_spinRulerLen->value();
    if (knownMm <= 0) return;
    m_rulerCalibration = pxDist / knownMm;
    m_lblRulerCalib->setText(QString("%1 px/mm").arg(m_rulerCalibration, 0, 'f', 2));
    log(QString("Ruler Calibrated: %1 px/mm").arg(m_rulerCalibration, 0, 'f', 2));
    updateRulerStats();
    initMosaicPanel(true);
}

void MainWindow::updateRulerStats()
{
    if (!m_hasRulerStart) return;
    QPointF end = m_hasRulerEnd ? m_rulerEnd : m_rulerStart;
    QLineF line(m_rulerStart, end);
    double pxDist = line.length();
    m_lblRulerPx->setText(QString("%1 px").arg(pxDist, 0, 'f', 1));
    if (m_rulerCalibration > 0)
        m_lblRulerMeas->setText(QString("%1 mm").arg(pxDist / m_rulerCalibration, 0, 'f', 2));
    else
        m_lblRulerMeas->setText("0.00 mm");

    if (m_chkShowProfile->isChecked() && !m_currentPixmap.isNull())
        updateIntensityProfile(m_rulerStart, end);
}

void MainWindow::onShowProfileToggled(bool checked)
{
    if (checked) {
        m_intensityChart->show();
        updateRulerStats();
    } else {
        m_intensityChart->hide();
    }
}

void MainWindow::updateIntensityProfile(QPointF p1, QPointF p2, const QImage *image)
{
    QImage qimg;
    if (image)
        qimg = *image;
    else if (!m_currentPixmap.isNull())
        qimg = m_currentPixmap.toImage();
    else
        return;

    QLineF line(p1, p2);
    int length = static_cast<int>(line.length());
    if (length < 2) return;

    QVector<int> data;
    data.reserve(length);
    for (int i = 0; i < length; ++i) {
        QPointF pt = line.pointAt(static_cast<double>(i) / length);
        int x = static_cast<int>(pt.x());
        int y = static_cast<int>(pt.y());
        if (x >= 0 && x < qimg.width() && y >= 0 && y < qimg.height()) {
            QColor col(qimg.pixel(x, y));
            data.append((col.red() + col.green() + col.blue()) / 3);
        }
    }
    m_intensityChart->setData(data);
}

// ---------- Color Picker ----------

void MainWindow::onColorPickerToggled(bool checked)
{
    m_colorPickerActive = checked;
    m_rightTabs->setTabVisible(m_colorPickerTabIndex, checked);
    if (checked) {
        m_rightTabs->setCurrentIndex(m_colorPickerTabIndex);
        if (m_rulerActive)
            m_actionRuler->setChecked(false);
    }
}

void MainWindow::updateColorPicker(const QPointF &pos)
{
    if (m_currentPixmap.isNull()) return;
    int x = static_cast<int>(pos.x());
    int y = static_cast<int>(pos.y());
    if (x >= 0 && x < m_currentPixmap.width() && y >= 0 && y < m_currentPixmap.height()) {
        QImage img = m_currentPixmap.toImage();
        QColor color(img.pixel(x, y));
        m_tabColorPicker->updateColor(x, y, color.red(), color.green(), color.blue());
    }
}

// ---------- Mosaic / Scan ----------

void MainWindow::initMosaicPanel(bool forceRecreate)
{
    if (m_mosaicPanel && !forceRecreate)
        return;

    if (m_rulerCalibration <= 0) {
        log("Cannot create Mosaic Panel: Ruler not calibrated.");
        return;
    }
    if (!m_stageSettings.contains("stage_width_mm")) {
        log("Cannot create Mosaic Panel: Stage settings not loaded.");
        return;
    }

    // Clear old contents of the mosaic tab
    auto *layout = qobject_cast<QVBoxLayout *>(m_mosaicTabContainer->layout());
    while (QLayoutItem *item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_mosaicPanel = nullptr;
    m_scanPanel = nullptr;

    double stageW = m_stageSettings["stage_width_mm"].toDouble();
    double stageH = m_stageSettings["stage_height_mm"].toDouble();
    m_mosaicPanel = new MosaicPanel(stageW, stageH, m_rulerCalibration);
    connect(m_mosaicPanel, &MosaicPanel::requestMove, this, &MainWindow::onMosaicMoveRequested);
    connect(m_mosaicPanel, &MosaicPanel::requestScan, this, &MainWindow::onMosaicScanRequested);

    layout->addWidget(m_mosaicPanel, 1);

    m_scanPanel = new ScanConfigPanel;
    connect(m_scanPanel, &ScanConfigPanel::startScanSignal, this, &MainWindow::startScan);
    connect(m_scanPanel, &ScanConfigPanel::cancelScanSignal, this, &MainWindow::cancelScan);
    layout->addWidget(m_scanPanel);
}

void MainWindow::onCncPositionUpdated(double x, double y, double /*z*/)
{
    m_currentCncXMm = x;
    m_currentCncYMm = y;
    if (m_mosaicPanel)
        m_mosaicPanel->setCncPosition(x, y);
}

void MainWindow::onCncStateUpdated(const QString &state)
{
    m_cncState = state;
}

void MainWindow::onMosaicMoveRequested(double x, double y)
{
    if (!m_cncControlPanel) return;
    int feedrate = m_cncControlPanel->feedrate();
    m_cncControlPanel->sendCommand(QString("G90 G1 X%1 Y%2 F%3")
                                       .arg(x, 0, 'f', 3).arg(y, 0, 'f', 3).arg(feedrate));
    log(QString("Mosaic Click: Moving to X=%1, Y=%2").arg(x, 0, 'f', 3).arg(y, 0, 'f', 3));
}

void MainWindow::onMosaicScanRequested(double xMin, double yMin, double xMax, double yMax)
{
    if (m_isScanning) { log("Scan already in progress."); return; }
    if (!m_cncControlPanel) { log("CNC panel not available."); return; }
    if (m_rulerCalibration <= 0) { log("Scan Error: Ruler not calibrated."); return; }
    if (m_currentPixmap.isNull()) { log("Scan Error: No camera image."); return; }

    if (m_scanPanel) {
        QVector<QRectF> areas;
        areas.append(QRectF(xMin, yMin, xMax - xMin, yMax - yMin));
        m_scanPanel->updateScanAreas(areas);
        log("Scan area selected. Configure and start scan in the 'Stage Control' panel.");
    }
}

void MainWindow::startScan(const QVector<QRectF> &areas, bool homeX, bool homeY,
                            bool serpentine, int feedrate)
{
    if (!m_cncControlPanel || areas.isEmpty()) return;
    QRectF area = areas.first();

    int imgW = m_currentPixmap.width();
    int imgH = m_currentPixmap.height();

    m_scanXMin = area.x();
    m_scanYMin = area.y();
    m_scanXMax = area.x() + area.width();
    m_scanYMax = area.y() + area.height();
    m_scanHomeX = homeX;
    m_scanHomeY = homeY;
    m_scanSerpentine = serpentine;
    m_scanFeedrate = feedrate;

    m_scanFovXMm = imgH / m_rulerCalibration;
    m_scanFovYMm = imgW / m_rulerCalibration;
    m_scanStepY = m_scanFovYMm * 0.75;

    double scanHeight = m_scanYMax - m_scanYMin;
    m_scanTotalRows = m_scanStepY > 0 ? static_cast<int>(scanHeight / m_scanStepY) : 0;
    m_scanCurrentRow = 0;
    m_scanCurrentY = m_scanYMax - (m_scanFovYMm / 2.0);
    m_scanIsFirstStrip = true;
    m_isScanning = true;

    m_cncControlPanel->sendCommand("G90");
    m_cncControlPanel->sendCommand(QString("F%1").arg(m_scanFeedrate));

    log(QString("Starting Mosaic Scan: %1 rows.").arg(m_scanTotalRows));
    if (m_scanPanel) {
        m_scanPanel->updateStatus(QString("Starting scan of %1 rows.").arg(m_scanTotalRows));
        m_scanPanel->updateProgress(0, m_scanTotalRows);
    }

    scanNextRow();
}

void MainWindow::scanNextRow()
{
    if (!m_isScanning || !m_cncControlPanel) return;

    if (m_scanCurrentY > m_scanYMin) {
        double yTarget = std::max(m_scanCurrentY, m_scanYMin + m_scanFovYMm / 2.0);
        double startX = m_scanXMin + (m_scanFovXMm / 2.0);
        double endX = m_scanXMax - (m_scanFovXMm / 2.0);

        if (m_scanIsFirstStrip) {
            m_cncControlPanel->sendCommand(QString("G1 X%1 Y%2").arg(startX, 0, 'f', 3).arg(yTarget, 0, 'f', 3));
            m_scanIsFirstStrip = false;
        } else {
            m_cncControlPanel->sendCommand(QString("G1 Y%1").arg(yTarget, 0, 'f', 3));
        }

        if (m_scanHomeY)
            m_cncControlPanel->sendCommand("$HY");
        if (m_scanHomeX)
            m_cncControlPanel->sendCommand("$HX");

        m_cncControlPanel->sendCommand(QString("G1 X%1 Y%2").arg(startX, 0, 'f', 3).arg(yTarget, 0, 'f', 3));
        m_cncControlPanel->sendCommand("G4 P0");
        m_cncControlPanel->sendCommand(QString("G1 X%1 Y%2").arg(endX, 0, 'f', 3).arg(yTarget, 0, 'f', 3));
        m_cncControlPanel->sendCommand("G4 P0");
        m_cncControlPanel->sendCommand("__SCAN_ROW_END__");

        m_scanCurrentY -= m_scanStepY;
    } else {
        m_cncControlPanel->sendCommand("__SCAN_DONE__");
    }
}

void MainWindow::onRowFinished()
{
    if (!m_isScanning) return;
    m_scanCurrentRow++;
    if (m_scanPanel)
        m_scanPanel->updateProgress(m_scanCurrentRow, m_scanTotalRows);
    QTimer::singleShot(100, this, &MainWindow::scanNextRow);
}

void MainWindow::cancelScan()
{
    if (!m_isScanning) return;
    m_isScanning = false;
    if (m_cncControlPanel) {
        m_cncControlPanel->sendCommand("!");
        m_cncControlPanel->stop();
    }
    log("Scan cancelled by user.");
    if (m_scanPanel)
        m_scanPanel->scanFinished(false);
}

void MainWindow::onScanFinished()
{
    if (!m_isScanning) return;
    m_isScanning = false;
    log("Mosaic scan finished.");
    if (m_scanPanel)
        m_scanPanel->scanFinished(true);
}

// ---------- Logging ----------

void MainWindow::log(const QString &message)
{
    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    m_logTextEdit->appendPlainText(QString("[%1] %2").arg(timestamp, message));
}

// ---------- Settings ----------

void MainWindow::loadSettings()
{
    QFile file("camera_settings.json");
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        QJsonObject settings = doc.object();

        m_spinExposureTime->setValue(settings.value("exposure_time").toDouble(2000));
        m_spinGain->setValue(settings.value("gain").toInt(1));
        m_chkAutoExposure->setChecked(settings.value("auto_exposure").toBool(true));

        if (settings.contains("ruler_calibration")) {
            m_rulerCalibration = settings.value("ruler_calibration").toDouble();
            if (m_rulerCalibration > 0)
                m_lblRulerCalib->setText(QString("%1 px/mm").arg(m_rulerCalibration, 0, 'f', 2));
        }

        if (settings.contains("led_controller_port"))
            m_ledController->setPort(settings.value("led_controller_port").toString());

        log("Settings loaded.");
    }

    loadStageSettings();
    initMosaicPanel();
}

void MainWindow::loadStageSettings()
{
    QFile file("stage_settings.json");
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        QJsonObject obj = doc.object();
        m_stageSettings["stage_width_mm"] = obj.value("stage_width_mm").toDouble();
        m_stageSettings["stage_height_mm"] = obj.value("stage_height_mm").toDouble();
        log("Stage settings loaded.");
    }
}

void MainWindow::saveSettings()
{
    QJsonObject settings;
    settings["exposure_time"] = m_spinExposureTime->value();
    settings["gain"] = m_spinGain->value();
    settings["auto_exposure"] = m_chkAutoExposure->isChecked();
    settings["ruler_calibration"] = m_rulerCalibration;
    settings["led_controller_port"] = m_ledController->getPort();
    if (m_cncControlPanel)
        settings["cnc_controller_port"] = QString(); // placeholder

    QFile file("camera_settings.json");
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(settings).toJson(QJsonDocument::Indented));
        log("Settings saved.");
    }
}
