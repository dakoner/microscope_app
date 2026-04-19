#ifndef YOLOINFERENCEWORKER_H
#define YOLOINFERENCEWORKER_H

#include <QObject>
#include <QImage>
#include <QMutex>
#include <deque>
#include <memory>

struct Detection {
    int x, y, w, h;  // Bounding box (x, y, width, height)
    float conf;      // Confidence score
};

/**
 * YOLOInferenceWorker: Runs YOLO inference in a background thread.
 *
 * Uses a TensorRT engine loaded through the TensorRT runtime.
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

private:
    struct Impl;

    QString resolveModelPath() const;
    std::vector<Detection> runInference(const QImage &frame, float confThreshold);
    static float intersectionOverUnion(const Detection &a, const Detection &b);
    static void applyNms(std::vector<Detection> &detections, float iouThreshold);

    std::unique_ptr<Impl> m_impl;
    bool m_isRunning = false;

    // Thread-safe storage of latest detections
    mutable QMutex m_detectionsMutex;
    std::vector<Detection> m_latestDetections;

    // Frame skip counter
    int m_frameSkipCounter = 0;
    int m_frameSkipRate = 1;  // Process every Nth frame
    uint64_t m_processedFrameIndex = 0;
};

#endif // YOLOINFERENCEWORKER_H
