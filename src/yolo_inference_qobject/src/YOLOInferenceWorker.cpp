#include "YOLOInferenceWorker.h"
#include <QApplication>
#include <QDebug>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <vector>

#pragma push_macro("slots")
#undef slots
#include <NvInferRuntime.h>
#include <cuda_runtime_api.h>
#pragma pop_macro("slots")

namespace {
constexpr int kInputSize = 640;
constexpr float kNmsIouThreshold = 0.45f;

size_t dataTypeBytes(nvinfer1::DataType type)
{
    switch (type) {
    case nvinfer1::DataType::kFLOAT: return 4;
    case nvinfer1::DataType::kHALF: return 2;
    case nvinfer1::DataType::kINT8: return 1;
    case nvinfer1::DataType::kINT32: return 4;
    case nvinfer1::DataType::kINT64: return 8;
    case nvinfer1::DataType::kBOOL: return 1;
    case nvinfer1::DataType::kUINT8: return 1;
    default: return 0;
    }
}

bool hasDynamicDims(const nvinfer1::Dims &dims)
{
    for (int i = 0; i < dims.nbDims; ++i) {
        if (dims.d[i] < 0) {
            return true;
        }
    }
    return false;
}

size_t elementCount(const nvinfer1::Dims &dims)
{
    if (dims.nbDims <= 0) {
        return 0;
    }
    size_t total = 1;
    for (int i = 0; i < dims.nbDims; ++i) {
        if (dims.d[i] <= 0) {
            return 0;
        }
        total *= static_cast<size_t>(dims.d[i]);
    }
    return total;
}

float toProbability(float raw)
{
    if (!std::isfinite(raw)) {
        return 0.0f;
    }

    if (raw >= 0.0f && raw <= 1.0f) {
        return raw;
    }

    // Convert logits to probabilities for TensorRT exports that omit sigmoid.
    const float clamped = std::clamp(raw, -80.0f, 80.0f);
    return 1.0f / (1.0f + std::exp(-clamped));
}

template <typename Getter>
void decodeRows(
    const Getter &get,
    int rows,
    int cols,
    float confThreshold,
    int imageWidth,
    int imageHeight,
    std::vector<Detection> &detections)
{
    const float xScale = static_cast<float>(imageWidth) / static_cast<float>(kInputSize);
    const float yScale = static_cast<float>(imageHeight) / static_cast<float>(kInputSize);

    for (int i = 0; i < rows; ++i) {
        if (cols < 6) {
            continue;
        }

        float x1 = 0.0f;
        float y1 = 0.0f;
        float x2 = 0.0f;
        float y2 = 0.0f;
        float score = 0.0f;

        const bool likelyCorners =
            (cols == 6) ||
            ((cols < 20) && (get(i, 2) > get(i, 0)) && (get(i, 3) > get(i, 1)));

        if (likelyCorners) {
            x1 = get(i, 0);
            y1 = get(i, 1);
            x2 = get(i, 2);
            y2 = get(i, 3);
            score = toProbability(get(i, 4));
        } else {
            const float cx = get(i, 0);
            const float cy = get(i, 1);
            const float bw = get(i, 2);
            const float bh = get(i, 3);

            const bool hasObjectness = (cols >= 6);
            const float objScore = hasObjectness ? toProbability(get(i, 4)) : 1.0f;
            const int classStart = hasObjectness ? 5 : 4;

            float bestClassScore = 0.0f;
            for (int c = classStart; c < cols; ++c) {
                bestClassScore = std::max(bestClassScore, toProbability(get(i, c)));
            }

            score = objScore * bestClassScore;

            x1 = cx - bw * 0.5f;
            y1 = cy - bh * 0.5f;
            x2 = cx + bw * 0.5f;
            y2 = cy + bh * 0.5f;
        }

        if (score < confThreshold) {
            continue;
        }

        const float maxAbs = std::max({
            std::fabs(x1), std::fabs(y1), std::fabs(x2), std::fabs(y2)
        });
        const bool normalized = maxAbs <= 2.0f;

        if (normalized) {
            x1 *= static_cast<float>(imageWidth);
            y1 *= static_cast<float>(imageHeight);
            x2 *= static_cast<float>(imageWidth);
            y2 *= static_cast<float>(imageHeight);
        } else {
            x1 *= xScale;
            y1 *= yScale;
            x2 *= xScale;
            y2 *= yScale;
        }

        x1 = std::clamp(x1, 0.0f, static_cast<float>(imageWidth));
        y1 = std::clamp(y1, 0.0f, static_cast<float>(imageHeight));
        x2 = std::clamp(x2, 0.0f, static_cast<float>(imageWidth));
        y2 = std::clamp(y2, 0.0f, static_cast<float>(imageHeight));

        Detection det;
        det.x = static_cast<int>(std::round(x1));
        det.y = static_cast<int>(std::round(y1));
        det.w = std::max(0, static_cast<int>(std::round(x2 - x1)));
        det.h = std::max(0, static_cast<int>(std::round(y2 - y1)));
        det.conf = score;
        if (det.w > 0 && det.h > 0) {
            detections.push_back(det);
        }
    }
}

void parseOutputTensor(
    const float *data,
    size_t elementCount,
    const nvinfer1::Dims &dims,
    float confThreshold,
    int imageWidth,
    int imageHeight,
    std::vector<Detection> &detections)
{
    if (!data || elementCount == 0) {
        return;
    }

    if (dims.nbDims == 3 && dims.d[0] == 1 && dims.d[1] >= 6 && dims.d[2] > 0) {
        const int d1 = dims.d[1];
        const int d2 = dims.d[2];

        // Common Ultralytics TensorRT output is [1, C, N] (channels-first),
        // where C is small (e.g. 84) and N is large (e.g. 8400).
        if (d2 > d1 && d1 <= 512) {
            const int cols = d1;
            const int rows = d2;
            auto get = [data, rows](int r, int c) -> float {
                return data[static_cast<size_t>(c) * static_cast<size_t>(rows) + static_cast<size_t>(r)];
            };
            decodeRows(get, rows, cols, confThreshold, imageWidth, imageHeight, detections);
            return;
        }

        // Fallback: treat as [1, N, C].
        if (d2 >= 6) {
            const int rows = d1;
            const int cols = d2;
            auto get = [data, cols](int r, int c) -> float {
                return data[static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(c)];
            };
            decodeRows(get, rows, cols, confThreshold, imageWidth, imageHeight, detections);
            return;
        }
    }

    if (dims.nbDims == 2 && dims.d[0] > 0 && dims.d[1] >= 6) {
        const int rows = dims.d[0];
        const int cols = dims.d[1];
        auto get = [data, cols](int r, int c) -> float {
            return data[static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(c)];
        };
        decodeRows(get, rows, cols, confThreshold, imageWidth, imageHeight, detections);
        return;
    }

    if (elementCount % 6 == 0) {
        const int rows = static_cast<int>(elementCount / 6);
        const int cols = 6;
        auto get = [data, cols](int r, int c) -> float {
            return data[static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(c)];
        };
        decodeRows(get, rows, cols, confThreshold, imageWidth, imageHeight, detections);
    }
}
}

struct YOLOInferenceWorker::Impl {
    class TrtLogger : public nvinfer1::ILogger {
    public:
        void log(Severity severity, const char *msg) noexcept override
        {
            if (severity <= Severity::kWARNING) {
                qWarning() << "TensorRT:" << msg;
            }
        }
    } logger;

    struct IoBuffer {
        QString name;
        bool isInput = false;
        nvinfer1::Dims dims{};
        nvinfer1::DataType type = nvinfer1::DataType::kFLOAT;
        size_t elements = 0;
        size_t bytes = 0;
        void *devicePtr = nullptr;
        std::vector<float> hostFloat;
    };

    std::unique_ptr<nvinfer1::IRuntime> runtime;
    std::unique_ptr<nvinfer1::ICudaEngine> engine;
    std::unique_ptr<nvinfer1::IExecutionContext> context;
    cudaStream_t stream = nullptr;

    QString modelPath;
    QString inputName;
    std::vector<IoBuffer> ioBuffers;
    std::vector<float> hostInput;
};

YOLOInferenceWorker::YOLOInferenceWorker(QObject *parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
}

YOLOInferenceWorker::~YOLOInferenceWorker()
{
    stop();
}

QString YOLOInferenceWorker::resolveModelPath() const
{
    const QString appDir = QApplication::applicationDirPath();
    QStringList tryPaths = {
        appDir + "/../models/train13/weights/best.engine",
        appDir + "/../../models/train13/weights/best.engine",
        appDir + "/models/train13/weights/best.engine",
        appDir + "/../models/best.engine",
        appDir + "/../../models/best.engine",
        appDir + "/models/best.engine"
    };

    if (appDir.contains("/release") || appDir.contains("/debug")) {
        tryPaths.prepend(appDir + "/../../../models/train13/weights/best.engine");
        tryPaths.append(appDir + "/../../../models/best.engine");
    }

    for (const QString &path : tryPaths) {
        if (QFileInfo::exists(path)) {
            return QFileInfo(path).absoluteFilePath();
        }
    }

    return QString();
}

void YOLOInferenceWorker::start()
{
    if (m_isRunning) return;

    m_impl->modelPath = resolveModelPath();
    if (m_impl->modelPath.isEmpty()) {
        emit errorOccurred("Could not find models/train13/weights/best.engine");
        return;
    }

    qDebug() << "YOLO loading TensorRT engine:" << m_impl->modelPath;

    std::ifstream engineFile(m_impl->modelPath.toStdString(), std::ios::binary);
    if (!engineFile) {
        emit errorOccurred(QString("Failed to open TensorRT engine file: %1").arg(m_impl->modelPath));
        return;
    }

    engineFile.seekg(0, std::ios::end);
    const std::streamoff size = engineFile.tellg();
    if (size <= 0) {
        emit errorOccurred("TensorRT engine file is empty.");
        return;
    }
    engineFile.seekg(0, std::ios::beg);

    std::vector<char> serialized(static_cast<size_t>(size));
    engineFile.read(serialized.data(), size);
    if (!engineFile) {
        emit errorOccurred("Failed to read TensorRT engine file.");
        return;
    }

    // Ultralytics TensorRT exports prepend metadata as:
    // [4-byte little-endian JSON length][JSON bytes][serialized TRT engine].
    const char *engineData = serialized.data();
    size_t engineSize = serialized.size();
    if (serialized.size() > 8) {
        uint32_t metaLen = 0;
        std::memcpy(&metaLen, serialized.data(), sizeof(metaLen));
        const size_t offset = static_cast<size_t>(sizeof(uint32_t)) + static_cast<size_t>(metaLen);

        if (metaLen > 0
            && offset < serialized.size()
            && serialized[sizeof(uint32_t)] == '{'
            && serialized[offset - 1] == '}') {
            engineData = serialized.data() + offset;
            engineSize = serialized.size() - offset;
            qDebug() << "YOLO detected Ultralytics TensorRT metadata header of" << metaLen
                     << "bytes; loading engine payload of" << engineSize << "bytes";
        }
    }

    m_impl->runtime.reset(nvinfer1::createInferRuntime(m_impl->logger));
    if (!m_impl->runtime) {
        emit errorOccurred("Failed to create TensorRT runtime.");
        return;
    }

    m_impl->engine.reset(m_impl->runtime->deserializeCudaEngine(engineData, engineSize));
    if (!m_impl->engine) {
        emit errorOccurred("Failed to deserialize TensorRT engine.");
        return;
    }

    m_impl->context.reset(m_impl->engine->createExecutionContext());
    if (!m_impl->context) {
        emit errorOccurred("Failed to create TensorRT execution context.");
        return;
    }

    if (cudaStreamCreate(&m_impl->stream) != cudaSuccess) {
        emit errorOccurred("Failed to create CUDA stream for TensorRT.");
        m_impl->context.reset();
        m_impl->engine.reset();
        m_impl->runtime.reset();
        return;
    }

    const int ioCount = m_impl->engine->getNbIOTensors();
    if (ioCount <= 0) {
        emit errorOccurred("TensorRT engine has no I/O tensors.");
        stop();
        return;
    }

    m_impl->inputName.clear();
    for (int i = 0; i < ioCount; ++i) {
        const char *tensorName = m_impl->engine->getIOTensorName(i);
        if (!tensorName) {
            continue;
        }

        if (m_impl->engine->getTensorIOMode(tensorName) == nvinfer1::TensorIOMode::kINPUT) {
            m_impl->inputName = QString::fromUtf8(tensorName);
            break;
        }
    }

    if (m_impl->inputName.isEmpty()) {
        emit errorOccurred("TensorRT engine has no input tensor.");
        stop();
        return;
    }

    nvinfer1::Dims4 inputShape{1, 3, kInputSize, kInputSize};
    if (!m_impl->context->setInputShape(m_impl->inputName.toUtf8().constData(), inputShape)) {
        emit errorOccurred("Failed to set TensorRT input shape to [1, 3, 640, 640].");
        stop();
        return;
    }

    m_impl->ioBuffers.clear();
    for (int i = 0; i < ioCount; ++i) {
        const char *tensorName = m_impl->engine->getIOTensorName(i);
        if (!tensorName) {
            continue;
        }

        Impl::IoBuffer buffer;
        buffer.name = QString::fromUtf8(tensorName);
        buffer.isInput = (m_impl->engine->getTensorIOMode(tensorName) == nvinfer1::TensorIOMode::kINPUT);
        buffer.type = m_impl->engine->getTensorDataType(tensorName);
        buffer.dims = m_impl->context->getTensorShape(tensorName);

        if (hasDynamicDims(buffer.dims)) {
            emit errorOccurred(QString("TensorRT tensor '%1' has unresolved dynamic shape.").arg(buffer.name));
            stop();
            return;
        }

        buffer.elements = elementCount(buffer.dims);
        const size_t typeSize = dataTypeBytes(buffer.type);
        if (buffer.elements == 0 || typeSize == 0) {
            emit errorOccurred(QString("Unsupported TensorRT tensor layout for '%1'.").arg(buffer.name));
            stop();
            return;
        }

        buffer.bytes = buffer.elements * typeSize;
        if (cudaMalloc(&buffer.devicePtr, buffer.bytes) != cudaSuccess) {
            emit errorOccurred(QString("Failed to allocate CUDA memory for tensor '%1'.").arg(buffer.name));
            stop();
            return;
        }

        if (!m_impl->context->setTensorAddress(tensorName, buffer.devicePtr)) {
            emit errorOccurred(QString("Failed to bind TensorRT tensor '%1'.").arg(buffer.name));
            stop();
            return;
        }

        if (!buffer.isInput && buffer.type == nvinfer1::DataType::kFLOAT) {
            buffer.hostFloat.resize(buffer.elements);
        }

        m_impl->ioBuffers.push_back(std::move(buffer));
    }

    m_impl->hostInput.assign(static_cast<size_t>(3 * kInputSize * kInputSize), 0.0f);
    if (m_impl->hostInput.empty()) {
        emit errorOccurred("Failed to initialize TensorRT input buffer.");
        stop();
        return;
    }

    qDebug() << "YOLO TensorRT initialized with" << m_impl->ioBuffers.size() << "tensors";
    m_processedFrameIndex = 0;
    m_isRunning = true;
}

void YOLOInferenceWorker::stop()
{
    m_isRunning = false;

    for (Impl::IoBuffer &buffer : m_impl->ioBuffers) {
        if (buffer.devicePtr) {
            cudaFree(buffer.devicePtr);
            buffer.devicePtr = nullptr;
        }
        buffer.hostFloat.clear();
    }
    m_impl->ioBuffers.clear();
    m_impl->hostInput.clear();

    if (m_impl->stream) {
        cudaStreamDestroy(m_impl->stream);
        m_impl->stream = nullptr;
    }

    m_impl->context.reset();
    m_impl->engine.reset();
    m_impl->runtime.reset();
    m_processedFrameIndex = 0;
}

bool YOLOInferenceWorker::isRunning() const
{
    return m_isRunning && static_cast<bool>(m_impl->context);
}

void YOLOInferenceWorker::inferFrame(const QImage &frame, float confThreshold)
{
    if (!isRunning() || frame.isNull()) return;

    // Simple frame skipping: process every Nth frame
    m_frameSkipCounter++;
    if (m_frameSkipCounter < m_frameSkipRate) {
        return;
    }
    m_frameSkipCounter = 0;

    const std::vector<Detection> detections = runInference(frame, confThreshold);
    const uint64_t frameIndex = m_processedFrameIndex++;

    QStringList matches;
    matches.reserve(static_cast<qsizetype>(detections.size()));
    for (const Detection &det : detections) {
        matches.append(QString("[x=%1 y=%2 w=%3 h=%4 conf=%5]")
                           .arg(det.x)
                           .arg(det.y)
                           .arg(det.w)
                           .arg(det.h)
                           .arg(det.conf, 0, 'f', 3));
    }

    qDebug() << "YOLO processed frame" << frameIndex
             << "matches=" << detections.size()
             << "details:" << (matches.isEmpty() ? QStringLiteral("[]") : matches.join(QStringLiteral("; ")));

    {
        QMutexLocker lock(&m_detectionsMutex);
        m_latestDetections = detections;
    }

    emit detectionsReady(detections);
}

std::vector<Detection> YOLOInferenceWorker::getLatestDetections() const
{
    QMutexLocker lock(&m_detectionsMutex);
    return m_latestDetections;
}

std::vector<Detection> YOLOInferenceWorker::runInference(const QImage &frame, float confThreshold)
{
    std::vector<Detection> empty;
    if (!m_impl->context || !m_impl->stream || m_impl->ioBuffers.empty()) {
        return empty;
    }

    QImage rgb = frame.convertToFormat(QImage::Format_RGB888);
    QImage resized = rgb.scaled(kInputSize, kInputSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    Impl::IoBuffer *inputBuffer = nullptr;
    std::vector<Impl::IoBuffer *> outputBuffers;
    outputBuffers.reserve(m_impl->ioBuffers.size());

    for (Impl::IoBuffer &buffer : m_impl->ioBuffers) {
        if (buffer.isInput && buffer.name == m_impl->inputName) {
            inputBuffer = &buffer;
        } else if (!buffer.isInput) {
            outputBuffers.push_back(&buffer);
        }
    }

    if (!inputBuffer) {
        emit errorOccurred("TensorRT input tensor not available.");
        return empty;
    }

    if (inputBuffer->type != nvinfer1::DataType::kFLOAT) {
        emit errorOccurred("TensorRT input tensor is not float32; only float32 input is currently supported.");
        return empty;
    }

    const int area = kInputSize * kInputSize;
    for (int y = 0; y < kInputSize; ++y) {
        const uchar *row = resized.constScanLine(y);
        for (int x = 0; x < kInputSize; ++x) {
            const int pixel = x * 3;
            const int idx = y * kInputSize + x;
            m_impl->hostInput[static_cast<size_t>(idx)] = static_cast<float>(row[pixel]) / 255.0f;
            m_impl->hostInput[static_cast<size_t>(area + idx)] = static_cast<float>(row[pixel + 1]) / 255.0f;
            m_impl->hostInput[static_cast<size_t>(2 * area + idx)] = static_cast<float>(row[pixel + 2]) / 255.0f;
        }
    }

    if (cudaMemcpyAsync(
            inputBuffer->devicePtr,
            m_impl->hostInput.data(),
            inputBuffer->bytes,
            cudaMemcpyHostToDevice,
            m_impl->stream) != cudaSuccess) {
        emit errorOccurred("Failed to upload input tensor to CUDA memory.");
        return empty;
    }

    if (!m_impl->context->enqueueV3(m_impl->stream)) {
        emit errorOccurred("TensorRT inference enqueue failed.");
        return empty;
    }

    for (Impl::IoBuffer *output : outputBuffers) {
        if (!output || output->type != nvinfer1::DataType::kFLOAT) {
            continue;
        }

        if (cudaMemcpyAsync(
                output->hostFloat.data(),
                output->devicePtr,
                output->bytes,
                cudaMemcpyDeviceToHost,
                m_impl->stream) != cudaSuccess) {
            emit errorOccurred(QString("Failed to read TensorRT output tensor '%1'.").arg(output->name));
            return empty;
        }
    }

    if (cudaStreamSynchronize(m_impl->stream) != cudaSuccess) {
        emit errorOccurred("CUDA stream synchronization failed after TensorRT inference.");
        return empty;
    }

    std::vector<Detection> detections;
    for (Impl::IoBuffer *output : outputBuffers) {
        if (!output || output->type != nvinfer1::DataType::kFLOAT || output->hostFloat.empty()) {
            continue;
        }
        parseOutputTensor(
            output->hostFloat.data(),
            output->elements,
            output->dims,
            confThreshold,
            frame.width(),
            frame.height(),
            detections);
    }

    applyNms(detections, kNmsIouThreshold);
    return detections;
}

float YOLOInferenceWorker::intersectionOverUnion(const Detection &a, const Detection &b)
{
    const int ax2 = a.x + a.w;
    const int ay2 = a.y + a.h;
    const int bx2 = b.x + b.w;
    const int by2 = b.y + b.h;

    const int interX1 = std::max(a.x, b.x);
    const int interY1 = std::max(a.y, b.y);
    const int interX2 = std::min(ax2, bx2);
    const int interY2 = std::min(ay2, by2);

    const int interW = std::max(0, interX2 - interX1);
    const int interH = std::max(0, interY2 - interY1);
    const float interArea = static_cast<float>(interW * interH);

    const float aArea = static_cast<float>(a.w * a.h);
    const float bArea = static_cast<float>(b.w * b.h);
    const float unionArea = aArea + bArea - interArea;

    if (unionArea <= 0.0f) {
        return 0.0f;
    }
    return interArea / unionArea;
}

void YOLOInferenceWorker::applyNms(std::vector<Detection> &detections, float iouThreshold)
{
    std::sort(detections.begin(), detections.end(), [](const Detection &a, const Detection &b) {
        return a.conf > b.conf;
    });

    std::vector<Detection> kept;
    std::vector<bool> removed(detections.size(), false);

    for (size_t i = 0; i < detections.size(); ++i) {
        if (removed[i]) {
            continue;
        }
        kept.push_back(detections[i]);

        for (size_t j = i + 1; j < detections.size(); ++j) {
            if (removed[j]) {
                continue;
            }
            if (intersectionOverUnion(detections[i], detections[j]) > iouThreshold) {
                removed[j] = true;
            }
        }
    }

    detections.swap(kept);
}
