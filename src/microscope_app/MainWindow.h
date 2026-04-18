#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QSlider>
#include <QPushButton>
#include <QRadioButton>
#include <QButtonGroup>
#include <QGroupBox>
#include <QTabWidget>
#include <QDockWidget>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QTimer>
#include <QPixmap>
#include <QPointF>
#include <QImage>
#include <QElapsedTimer>
#include <cstdint>
#include <deque>
#include <vector>

namespace Ui { class MainWindow; }

class MindVisionCamera;
class VideoThread;
class CNCControlPanel;
class LEDController;
class MosaicPanel;
class IntensityChart;
class ScanConfigPanel;
class ColorPickerWidget;

// Include for Detection struct (needed for std::vector<Detection>)
#include "YOLOInferenceWorker.h"

class YOLOInferenceWorker;  // Forward declare again for the member pointer

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    // Camera
    void onStartClicked();
    void onStopClicked();
    void onRecordClicked();
    void onSnapshotClicked();
    void onHomeAndRunClicked();

    // Camera settings
    void onAutoExposureToggled(bool checked);
    void onRoiToggled(bool checked);
    void onExposureTimeChanged(double value);
    void onExposureSliderChanged(int value);
    void onGainChanged(int value);
    void onGainSliderChanged(int value);
    void onAeTargetChanged(int value);
    void onAeSliderChanged(int value);
    void onTriggerModeChanged(int id, bool checked);
    void onSoftTriggerClicked();
    void onTriggerCountChanged(int value);
    void onTriggerDelayChanged(int value);
    void onTriggerIntervalChanged(int value);
    void onExtModeChanged(int index);
    void onExtJitterChanged(int value);
    void onExtShutterChanged(int index);
    void onStrobeModeChanged(int index);
    void onStrobePolarityChanged(int index);
    void onStrobeDelayChanged(int value);
    void onStrobeWidthChanged(int value);
    void pollCameraParams();

    // Frame pipeline
    void updateFrame(QImage image, double frameTimestampSec);
    void updateFps(double fps);
    void handleError(QString message);

    // YOLO inference
    void onYoloToggled(bool checked);
    void onDetectionsReady(const std::vector<Detection> &detections);
    void onYoloError(const QString &message);

    // Ruler / Measurement
    void onRulerToggled(bool checked);
    void calibrateRuler();
    void onShowProfileToggled(bool checked);

    // Color Picker
    void onColorPickerToggled(bool checked);

    // Mosaic / Scan
    void onCncPositionUpdated(double x, double y, double z);
    void onCncStateUpdated(const QString &state);
    void onMosaicMoveRequested(double x, double y);
    void onMosaicScanRequested(double xMin, double yMin, double xMax, double yMax);
    void startScan(const QVector<QRectF> &areas, bool homeX, bool homeY,
                   bool serpentine, int feedrate);
    void scanNextRow();
    void onRowFinished();
    void cancelScan();
    void onScanFinished();

    void log(const QString &message);

private:
    void connectSignals();
    void toggleCenterViewTab();
    void repositionPipOverlays();
    void startScanRowRecording(int rowNumber);
    void stopScanRowRecording();
    double monotonicNowSec() const;
    void addPoseSample(double x, double y, double timestampSec);
    bool interpolatedPoseAt(double timestampSec, double &xOut, double &yOut) const;
    void syncUi();
    void updateSliderFromTime(double current, double minVal, double maxVal);
    void refreshVideoLabel();
    void updateFrameStatsLabel();
    QPointF getImageCoords(const QPointF &mousePos);
    void updateRulerStats();
    void updateIntensityProfile(QPointF p1, QPointF p2, const QImage *image = nullptr);
    void updateColorPicker(const QPointF &pos);
    void applyCameraSettings();
    void initMosaicPanel(bool forceRecreate = false);
    void loadSettings();
    void saveSettings();
    void loadStageSettings();

    // Camera
    MindVisionCamera *m_camera = nullptr;
    VideoThread *m_videoThread = nullptr;
    VideoThread *m_scanVideoThread = nullptr;
    bool m_isCameraRunning = false;
    bool m_recordingRequested = false;
    double m_currentFps = 30.0;
    double m_lastUiUpdateTime = 0.0;
    double m_lastMosaicUpdateTime = 0.0;
    std::uint64_t m_framesReceivedCount = 0;
    std::uint64_t m_framesWrittenToMosaicCount = 0;
    QPixmap m_currentPixmap;
    QImage m_currentImage;
    QSize m_lastVideoLabelSize;

    // Hardware
    CNCControlPanel *m_cncControlPanel = nullptr;
    LEDController *m_ledController = nullptr;

    // Mosaic / Scan
    MosaicPanel *m_mosaicPanel = nullptr;
    QWidget *m_mosaicTabContainer = nullptr;
    ScanConfigPanel *m_scanPanel = nullptr;
    double m_currentCncXMm = 0.0;
    double m_currentCncYMm = 0.0;
    QString m_cncState = "Idle";
    bool m_isScanning = false;
    int m_scanCurrentRow = 0;
    int m_scanTotalRows = 0;
    double m_scanXMin = 0, m_scanYMin = 0, m_scanXMax = 0, m_scanYMax = 0;
    bool m_scanHomeX = false, m_scanHomeY = false;
    bool m_scanSerpentine = false;
    int m_scanFeedrate = 500;
    double m_scanStepY = 0;
    double m_scanCurrentY = 0;
    bool m_scanIsFirstStrip = true;
    double m_scanFovXMm = 0, m_scanFovYMm = 0;
    QString m_scanVideoOutputDir;
    qint64 m_scanSessionTimestamp = 0;
    bool m_scanRowRecordingActive = false;
    int m_scanRecordingRowNumber = 0;

    struct PoseSample {
        double timestampSec = 0.0;
        double xMm = 0.0;
        double yMm = 0.0;
    };
    QElapsedTimer m_poseClock;
    std::deque<PoseSample> m_poseSamples;

    // YOLO Inference / Tardigrade tracking
    YOLOInferenceWorker *m_yoloWorker = nullptr;
    std::vector<Detection> m_latestDetections;
    bool m_yoloInferenceActive = false;
    float m_yoloConfThreshold = 0.5f;
    QAction *m_actionYoloInference = nullptr;

    // Ruler / Measurement
    bool m_rulerActive = false;
    QPointF m_rulerStart;
    QPointF m_rulerEnd;
    bool m_hasRulerStart = false;
    bool m_hasRulerEnd = false;
    double m_rulerCalibration = 0.0; // px/mm (0 = not calibrated)

    // Color picker
    bool m_colorPickerActive = false;

    // Stage settings
    QVariantMap m_stageSettings;

    // Timer
    QTimer m_paramPollTimer;

    // Ui
    Ui::MainWindow *ui = nullptr;

    // ---- UI elements ----
    QSplitter *m_mainSplitter = nullptr;

    // Video
    QTabWidget *m_centerTabs = nullptr;
    QLabel *m_videoLabel = nullptr;
    QLabel *m_videoPipLabel = nullptr;  // PiP in mosaic tab
    QLabel *m_mosaicPipLabel = nullptr; // PiP in video tab

    // Status bar
    QLabel *m_fpsLabel = nullptr;
    QLabel *m_framesReceivedLabel = nullptr;
    QLabel *m_framesMosaicLabel = nullptr;

    // Log
    QPlainTextEdit *m_logTextEdit = nullptr;

    // Camera controls
    QCheckBox *m_chkAutoExposure = nullptr;
    QCheckBox *m_chkRoi = nullptr;
    QDoubleSpinBox *m_spinExposureTime = nullptr;
    QSlider *m_sliderExposure = nullptr;
    QSpinBox *m_spinGain = nullptr;
    QSlider *m_sliderGain = nullptr;
    QSpinBox *m_spinAeTarget = nullptr;
    QSlider *m_sliderAeTarget = nullptr;
    QRadioButton *m_rbContinuous = nullptr;
    QRadioButton *m_rbSoftware = nullptr;
    QRadioButton *m_rbHardware = nullptr;
    QButtonGroup *m_triggerBg = nullptr;
    QPushButton *m_btnSoftTrigger = nullptr;
    QSpinBox *m_spinTriggerCount = nullptr;
    QSpinBox *m_spinTriggerDelay = nullptr;
    QSpinBox *m_spinTriggerInterval = nullptr;
    QComboBox *m_comboExtMode = nullptr;
    QSpinBox *m_spinExtJitter = nullptr;
    QComboBox *m_comboExtShutter = nullptr;
    QComboBox *m_comboStrobeMode = nullptr;
    QComboBox *m_comboStrobePolarity = nullptr;
    QSpinBox *m_spinStrobeDelay = nullptr;
    QSpinBox *m_spinStrobeWidth = nullptr;
    QGroupBox *m_controlsGroup = nullptr;
    QGroupBox *m_triggerGroup = nullptr;
    QGroupBox *m_triggerParamsGroup = nullptr;
    QGroupBox *m_extTriggerGroup = nullptr;
    QGroupBox *m_strobeGroup = nullptr;

    // Right panel
    QTabWidget *m_rightTabs = nullptr;

    // Ruler
    QSpinBox *m_spinRulerLen = nullptr;
    QLabel *m_lblRulerPx = nullptr;
    QLabel *m_lblRulerCalib = nullptr;
    QLabel *m_lblRulerMeas = nullptr;
    QPushButton *m_btnRulerCalibrate = nullptr;
    QCheckBox *m_chkShowProfile = nullptr;
    IntensityChart *m_intensityChart = nullptr;

    // Color Picker
    ColorPickerWidget *m_tabColorPicker = nullptr;
    int m_colorPickerTabIndex = -1;
    int m_rulerTabIndex = 0;

    // Actions
    QAction *m_actionStartCamera = nullptr;
    QAction *m_actionStopCamera = nullptr;
    QAction *m_actionRecord = nullptr;
    QAction *m_actionSnapshot = nullptr;
    QAction *m_actionRuler = nullptr;
    QAction *m_actionColorPicker = nullptr;
    QAction *m_actionHomeAndRun = nullptr;
};

#endif // MAINWINDOW_H
