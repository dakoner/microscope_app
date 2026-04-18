#include "YOLOInferenceWorker.h"
#include <QApplication>
#include <QFileInfo>
#include <QDir>
#include <QDebug>

#include <algorithm>
#include <cmath>

#pragma push_macro("slots")
#undef slots
#include <torch/script.h>
#ifdef HAS_TORCH_CUDA
#include <torch/cuda.h>
#endif
#pragma pop_macro("slots")

namespace {
constexpr int kInputSize = 640;
constexpr float kNmsIouThreshold = 0.45f;

std::vector<Detection> parsePredictions(
    const torch::Tensor &prediction,
    float confThreshold,
    int imageWidth,
    int imageHeight)
{
    std::vector<Detection> detections;

    torch::Tensor pred = prediction.detach().to(torch::kCPU);

    if (pred.dim() == 3 && pred.size(0) == 1) {
        pred = pred.squeeze(0);
    }

    if (pred.dim() == 2 && pred.size(0) > 0 && pred.size(1) > 0) {
        if (pred.size(1) >= 6 && pred.size(1) < 20) {
            // Likely post-NMS format [x1, y1, x2, y2, conf, cls].
            const int rows = static_cast<int>(pred.size(0));
            for (int i = 0; i < rows; ++i) {
                const float score = pred[i][4].item<float>();
                if (score < confThreshold) {
                    continue;
                }

                float x1 = pred[i][0].item<float>();
                float y1 = pred[i][1].item<float>();
                float x2 = pred[i][2].item<float>();
                float y2 = pred[i][3].item<float>();

                x1 = std::clamp(x1 * (static_cast<float>(imageWidth) / kInputSize), 0.0f, static_cast<float>(imageWidth));
                y1 = std::clamp(y1 * (static_cast<float>(imageHeight) / kInputSize), 0.0f, static_cast<float>(imageHeight));
                x2 = std::clamp(x2 * (static_cast<float>(imageWidth) / kInputSize), 0.0f, static_cast<float>(imageWidth));
                y2 = std::clamp(y2 * (static_cast<float>(imageHeight) / kInputSize), 0.0f, static_cast<float>(imageHeight));

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
            return detections;
        }

        // Raw YOLO format often comes as [N, 4 + num_classes].
        if (pred.size(1) > 6) {
            const int rows = static_cast<int>(pred.size(0));
            for (int i = 0; i < rows; ++i) {
                const torch::Tensor row = pred[i];

                const float cx = row[0].item<float>();
                const float cy = row[1].item<float>();
                const float bw = row[2].item<float>();
                const float bh = row[3].item<float>();

                const torch::Tensor clsScores = row.slice(0, 4, row.size(0));
                const auto maxPair = clsScores.max(0);
                const float score = std::get<0>(maxPair).item<float>();
                if (score < confThreshold) {
                    continue;
                }

                float x1 = (cx - bw * 0.5f) * (static_cast<float>(imageWidth) / kInputSize);
                float y1 = (cy - bh * 0.5f) * (static_cast<float>(imageHeight) / kInputSize);
                float x2 = (cx + bw * 0.5f) * (static_cast<float>(imageWidth) / kInputSize);
                float y2 = (cy + bh * 0.5f) * (static_cast<float>(imageHeight) / kInputSize);

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
            return detections;
        }
    }

    // Handle common exported shape [1, 84, 8400] or [84, 8400].
    if (pred.dim() == 2 && pred.size(0) > pred.size(1)) {
        pred = pred.transpose(0, 1);
        return parsePredictions(pred, confThreshold, imageWidth, imageHeight);
    }

    qWarning() << "Unsupported model output shape from TorchScript model.";
    return detections;
}
}

struct YOLOInferenceWorker::Impl {
    std::unique_ptr<torch::jit::script::Module> model;
    QString modelPath;
    torch::Device device = torch::kCPU;
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
        appDir + "/../models/train13/weights/best.torchscript",
        appDir + "/../../models/train13/weights/best.torchscript",
        appDir + "/models/train13/weights/best.torchscript",
        appDir + "/../models/best.torchscript",
        appDir + "/../../models/best.torchscript",
        appDir + "/models/best.torchscript"
    };

    if (appDir.contains("/release") || appDir.contains("/debug")) {
        tryPaths.prepend(appDir + "/../../../models/train13/weights/best.torchscript");
        tryPaths.append(appDir + "/../../../models/best.torchscript");
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
        emit errorOccurred("Could not find models/train13/weights/best.torchscript");
        return;
    }

    qInfo() << "YOLO loading model:" << m_impl->modelPath;

    try {
#ifdef HAS_TORCH_CUDA
        if (!torch::cuda::is_available()) {
            emit errorOccurred("YOLO GPU-only mode: CUDA runtime is not available.");
            return;
        }
        m_impl->model = std::make_unique<torch::jit::script::Module>(
            torch::jit::load(m_impl->modelPath.toStdString(), torch::kCUDA));
        m_impl->device = torch::kCUDA;
        qInfo() << "YOLO LibTorch using CUDA";
#else
        emit errorOccurred("YOLO GPU-only mode: application is not built with CUDA-enabled LibTorch.");
        return;
#endif

        m_impl->model->eval();
    } catch (const c10::Error &e) {
        m_impl->model.reset();
        emit errorOccurred(QString("Failed to load TorchScript model: %1").arg(e.what()));
        return;
    } catch (const std::exception &e) {
        m_impl->model.reset();
        emit errorOccurred(QString("Failed to initialize TorchScript runtime: %1").arg(e.what()));
        return;
    }

    m_isRunning = true;
}

void YOLOInferenceWorker::stop()
{
    if (!m_isRunning) return;

    m_isRunning = false;
    m_impl->model.reset();
}

bool YOLOInferenceWorker::isRunning() const
{
    return m_isRunning && static_cast<bool>(m_impl->model);
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
    if (!m_impl->model) {
        return empty;
    }

    try {
        QImage rgb = frame.convertToFormat(QImage::Format_RGB888);
        QImage resized = rgb.scaled(kInputSize, kInputSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

        torch::Tensor input = torch::from_blob(
            const_cast<uchar *>(resized.constBits()),
            {kInputSize, kInputSize, 3},
            torch::kUInt8);

        input = input.permute({2, 0, 1}).to(torch::kFloat32).div_(255.0f).unsqueeze(0).clone();

        if (m_impl->device.is_cuda()) {
            input = input.to(m_impl->device);
        }

        std::vector<torch::jit::IValue> inputs;
        inputs.emplace_back(input);

        torch::IValue outputValue = m_impl->model->forward(inputs);
        torch::Tensor pred;

        if (outputValue.isTensor()) {
            pred = outputValue.toTensor();
        } else if (outputValue.isTuple()) {
            const auto &elements = outputValue.toTupleRef().elements();
            for (const auto &elem : elements) {
                if (elem.isTensor()) {
                    pred = elem.toTensor();
                    break;
                }
            }
        } else if (outputValue.isList()) {
            auto list = outputValue.toListRef();
            for (const c10::IValue &elem : list) {
                if (elem.isTensor()) {
                    pred = elem.toTensor();
                    break;
                }
            }
        }

        if (!pred.defined()) {
            emit errorOccurred("TorchScript model returned unsupported output type.");
            return empty;
        }

        std::vector<Detection> detections = parsePredictions(pred, confThreshold, frame.width(), frame.height());
        applyNms(detections, kNmsIouThreshold);
        return detections;
    } catch (const c10::Error &e) {
        const QString msg = QString::fromUtf8(e.what());
        if (msg.contains("Expected all tensors to be on the same device", Qt::CaseInsensitive)) {
            emit errorOccurred(
                "LibTorch inference failed: model was exported with mixed CPU/CUDA tensors. "
                "GPU-only mode requires a CUDA-exported TorchScript model (export with device=0)."
            );
            return empty;
        }

        emit errorOccurred(QString("LibTorch inference failed: %1").arg(msg));
        return empty;
    } catch (const std::exception &e) {
        const QString msg = QString::fromUtf8(e.what());
        if (msg.contains("Expected all tensors to be on the same device", Qt::CaseInsensitive)) {
            emit errorOccurred(
                "LibTorch inference failed: model was exported with mixed CPU/CUDA tensors. "
                "GPU-only mode requires a CUDA-exported TorchScript model (export with device=0)."
            );
            return empty;
        }

        emit errorOccurred(QString("LibTorch inference failed: %1").arg(msg));
        return empty;
    }
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
