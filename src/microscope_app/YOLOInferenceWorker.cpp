#include "YOLOInferenceWorker.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QBuffer>
#include <QImageWriter>
#include <QApplication>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QDir>

static QString resolveEmbeddedPythonExecutable()
{
    const QStringList candidates = {
        "/home/davidek/.local/share/uv/python/cpython-3.11.15-linux-x86_64-gnu/bin/python3.11",
        "/home/davidek/.local/share/uv/python/cpython-3.11.15-linux-x86_64-gnu/bin/python3",
        "/home/davidek/.local/share/uv/python/cpython-3.11-linux-x86_64-gnu/bin/python3"
    };
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path)) {
            return path;
        }
    }
    return "python3";  // Fallback to system python3
}

static QString resolveYoloPythonExecutable(const QString &scriptPath)
{
    const QString scriptDir = QFileInfo(scriptPath).absolutePath();
    const QString venvEnv = qEnvironmentVariable("VIRTUAL_ENV");

    const QStringList candidates = {
        scriptDir + "/.venv/bin/python3",
        scriptDir + "/.venv/bin/python",
        scriptDir + "/.venv311/bin/python3",
        scriptDir + "/.venv311/bin/python",
        scriptDir + "/.venv_sys/bin/python3",
        scriptDir + "/.venv_sys/bin/python",
        venvEnv.isEmpty() ? QString() : venvEnv + "/bin/python3",
        venvEnv.isEmpty() ? QString() : venvEnv + "/bin/python"
    };

    for (const QString &path : candidates) {
        if (!path.isEmpty() && QFileInfo::exists(path)) {
            return path;
        }
    }

    return resolveEmbeddedPythonExecutable();
}

YOLOInferenceWorker::YOLOInferenceWorker(QObject *parent)
    : QObject(parent)
{
    m_process = new QProcess(this);
    
    // Connect subprocess signals
    connect(m_process, &QProcess::readyReadStandardOutput, this, &YOLOInferenceWorker::onProcessReadyReadStandardOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, &YOLOInferenceWorker::onProcessReadyReadStandardError);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &YOLOInferenceWorker::onProcessFinished);
    #if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    connect(m_process, &QProcess::errorOccurred, this, &YOLOInferenceWorker::onProcessError);
    #else
    connect(m_process, static_cast<void(QProcess::*)(QProcess::ProcessError)>(&QProcess::error),
            this, &YOLOInferenceWorker::onProcessError);
    #endif
}

YOLOInferenceWorker::~YOLOInferenceWorker()
{
    stop();
}

void YOLOInferenceWorker::start()
{
    if (m_isRunning) return;

    // Find the Python script in the application directory
    QString appDir = QApplication::applicationDirPath();
    QString scriptPath;
    
    // Try multiple locations
    QStringList tryPaths = {
        appDir + "/../yolo_inference.py",  // ../yolo_inference.py (relative to build dir)
        appDir + "/../../yolo_inference.py",  // ../../yolo_inference.py (from release subdir)
        appDir + "/yolo_inference.py"         // Same directory
    };
    
    // Also try the source directory structure
    if (appDir.contains("/release") || appDir.contains("/debug")) {
        // Build directory - go up to source root
        tryPaths.prepend(appDir + "/../../../yolo_inference.py");
    }
    
    for (const QString &path : tryPaths) {
        if (QFileInfo::exists(path)) {
            scriptPath = QFileInfo(path).absoluteFilePath();
            break;
        }
    }
    
    if (scriptPath.isEmpty()) {
        emit errorOccurred("Could not find yolo_inference.py script");
        return;
    }

    // Start Python subprocess with environment that matches available packages
    QString pythonExe = resolveYoloPythonExecutable(scriptPath);
    
    // Set up environment for the subprocess
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString pyHome = "/home/davidek/.local/share/uv/python/cpython-3.11.15-linux-x86_64-gnu";
    if (!QDir(pyHome).exists()) {
        pyHome = "/home/davidek/.local/share/uv/python/cpython-3.11-linux-x86_64-gnu";
    }
    
    const bool usingEmbeddedPython = pythonExe.startsWith("/home/davidek/.local/share/uv/python/");

    if (usingEmbeddedPython && QDir(pyHome).exists()) {
        QString stdlib = pyHome + "/lib/python3.11";
        QString dynload = stdlib + "/lib-dynload";
        QString site = stdlib + "/site-packages";
        QString pyPath = stdlib + ":" + dynload + ":" + site;
        
        env.insert("PYTHONHOME", pyHome);
        env.insert("PYTHONPATH", pyPath);
        // Ensure consistent Python environment
        env.insert("PYTHONDONTWRITEBYTECODE", "1");
    } else {
        // Let venv/system Python manage its own stdlib and site-packages.
        env.remove("PYTHONHOME");
        env.remove("PYTHONPATH");
    }
    
    m_process->setProcessEnvironment(env);
    
    QStringList args = {scriptPath};
    m_process->start(pythonExe, args);

    if (!m_process->waitForStarted()) {
        emit errorOccurred(QString("Failed to start inference process: %1").arg(m_process->errorString()));
        return;
    }

    m_isRunning = true;
    m_outputBuffer.clear();
}

void YOLOInferenceWorker::stop()
{
    if (!m_isRunning) return;

    m_isRunning = false;
    m_process->terminate();

    if (!m_process->waitForFinished(3000)) {
        m_process->kill();
        m_process->waitForFinished();
    }
}

bool YOLOInferenceWorker::isRunning() const
{
    return m_isRunning && m_process->state() == QProcess::Running;
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

    sendFrameToInference(frame, confThreshold);
}

std::vector<Detection> YOLOInferenceWorker::getLatestDetections() const
{
    QMutexLocker lock(&m_detectionsMutex);
    return m_latestDetections;
}

void YOLOInferenceWorker::sendFrameToInference(const QImage &frame, float confThreshold)
{
    // Encode frame as JPEG in memory
    QImage rgbFrame = frame.convertToFormat(QImage::Format_RGB888);
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    
    QImageWriter writer(&buffer, "JPEG");
    writer.setQuality(85);  // Trade-off between quality and speed
    
    if (!writer.write(rgbFrame)) {
        emit errorOccurred("Failed to encode frame to JPEG");
        return;
    }
    buffer.close();

    // Encode as base64
    QByteArray jpegData = buffer.data();
    QString base64Frame = QString::fromLatin1(jpegData.toBase64());

    // Create JSON input
    QJsonObject frameObj;
    frameObj["frame"] = base64Frame;
    frameObj["conf_threshold"] = static_cast<double>(confThreshold);
    frameObj["width"] = frame.width();
    frameObj["height"] = frame.height();

    QJsonDocument doc(frameObj);
    QString jsonLine = QString::fromUtf8(doc.toJson(QJsonDocument::Compact)) + "\n";

    // Send to Python process
    m_process->write(jsonLine.toUtf8());
}

void YOLOInferenceWorker::onProcessReadyReadStandardOutput()
{
    // Read output line-by-line
    while (m_process->canReadLine()) {
        QString line = QString::fromUtf8(m_process->readLine()).trimmed();
        if (line.isEmpty()) continue;

        // Try to parse as JSON detection result
        QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8());
        if (!doc.isObject()) continue;

        QJsonObject obj = doc.object();
        QJsonArray detectionsArray = obj["detections"].toArray();

        std::vector<Detection> detections;
        for (const QJsonValue &val : detectionsArray) {
            QJsonObject detObj = val.toObject();
            Detection det;
            det.x = detObj["x"].toInt();
            det.y = detObj["y"].toInt();
            det.w = detObj["w"].toInt();
            det.h = detObj["h"].toInt();
            det.conf = static_cast<float>(detObj["conf"].toDouble());
            
            detections.push_back(det);
        }

        // Update latest detections (thread-safe)
        {
            QMutexLocker lock(&m_detectionsMutex);
            m_latestDetections = detections;
        }

        // Emit signal
        emit detectionsReady(detections);
    }
}

void YOLOInferenceWorker::onProcessReadyReadStandardError()
{
    QString errorOutput = QString::fromUtf8(m_process->readAllStandardError());
    // Print to stderr for debugging, but don't treat as fatal
    qWarning() << "YOLO Worker stderr:" << errorOutput;
}

void YOLOInferenceWorker::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    m_isRunning = false;
    QString msg = QString("Inference process finished with exit code %1").arg(exitCode);
    emit errorOccurred(msg);
}

void YOLOInferenceWorker::onProcessError(QProcess::ProcessError error)
{
    m_isRunning = false;
    QString errorMsg;
    switch (error) {
        case QProcess::FailedToStart:
            errorMsg = "Failed to start inference process";
            break;
        case QProcess::Crashed:
            errorMsg = "Inference process crashed";
            break;
        case QProcess::Timedout:
            errorMsg = "Inference process timeout";
            break;
        default:
            errorMsg = "Inference process error";
    }
    emit errorOccurred(errorMsg);
}
