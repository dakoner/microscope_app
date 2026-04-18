#ifndef YOLOINFERENCEWORKER_H
#define YOLOINFERENCEWORKER_H

#include <QObject>
#include <QProcess>
#include <QImage>
#include <QMutex>
#include <deque>

struct Detection {
    int x, y, w, h;  // Bounding box (x, y, width, height)
    float conf;      // Confidence score
};

/**
 * YOLOInferenceWorker: Runs YOLO inference in a background thread.
 * 
 * Communicates with a Python subprocess via JSON on stdin/stdout.
 * Frames are sent for inference with optional frame skipping to match inference speed.
 */
class YOLOInferenceWorker : public QObject
{
    Q_OBJECT

public:
    explicit YOLOInferenceWorker(QObject *parent = nullptr);
    ~YOLOInferenceWorker() override;

    // Start/stop inference process
    void start();
    void stop();
    bool isRunning() const;

    // Queue a frame for inference
    void inferFrame(const QImage &frame, float confThreshold = 0.5f);

    // Get latest detections (thread-safe)
    std::vector<Detection> getLatestDetections() const;

signals:
    // Emitted when detections are available for a frame
    void detectionsReady(const std::vector<Detection> &detections);

    // Emitted on errors
    void errorOccurred(const QString &message);

private slots:
    void onProcessReadyReadStandardOutput();
    void onProcessReadyReadStandardError();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);

private:
    void sendFrameToInference(const QImage &frame, float confThreshold);

    QProcess *m_process = nullptr;
    bool m_isRunning = false;
    
    // Thread-safe storage of latest detections
    mutable QMutex m_detectionsMutex;
    std::vector<Detection> m_latestDetections;
    
    // Output buffering for reading JSON responses
    QString m_outputBuffer;
    
    // Frame skip counter
    int m_frameSkipCounter = 0;
    int m_frameSkipRate = 1;  // Process every Nth frame
};

#endif // YOLOINFERENCEWORKER_H
