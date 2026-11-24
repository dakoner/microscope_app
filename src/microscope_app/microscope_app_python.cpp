#include <pybind11/pybind11.h>

#include "MainWindow.h"
#include "MindVisionCamera.h"
#include "MindVisionCameraServer.h"

#include <QDateTime>
#include <cstdint>
#include <iostream>
#include <optional>

namespace py = pybind11;

PYBIND11_MODULE(_microscope_app_cpp, m)
{
    m.doc() = "Pybind11 bridge for C++ microscope MainWindow and headless CameraServer";

    m.def("create_main_window_ptr", []() {
        auto *window = new MainWindow();
        return reinterpret_cast<std::uintptr_t>(window);
    });

    m.def("show_maximized_ptr", [](std::uintptr_t ptr) {
        auto *window = reinterpret_cast<MainWindow *>(ptr);
        if (window) {
            window->showMaximized();
        }
    });

    m.def("delete_main_window_ptr", [](std::uintptr_t ptr) {
        auto *window = reinterpret_cast<MainWindow *>(ptr);
        delete window;
    });

    m.def("mcp_status_ptr", [](std::uintptr_t ptr) {
        return reinterpret_cast<MainWindow *>(ptr)->mcpStatusJson();
    });
    m.def("mcp_start_camera_ptr", [](std::uintptr_t ptr) {
        return reinterpret_cast<MainWindow *>(ptr)->mcpStartCameraJson();
    });
    m.def("mcp_stop_camera_ptr", [](std::uintptr_t ptr) {
        return reinterpret_cast<MainWindow *>(ptr)->mcpStopCameraJson();
    });
    m.def("mcp_snapshot_ptr", [](std::uintptr_t ptr) {
        return reinterpret_cast<MainWindow *>(ptr)->mcpSnapshotJson();
    });
    m.def("mcp_set_recording_ptr", [](std::uintptr_t ptr, bool enabled) {
        return reinterpret_cast<MainWindow *>(ptr)->mcpSetRecordingJson(enabled);
    });
    m.def("mcp_set_exposure_ptr", [](std::uintptr_t ptr, double milliseconds) {
        return reinterpret_cast<MainWindow *>(ptr)->mcpSetExposureJson(milliseconds);
    });
    m.def("mcp_set_gain_ptr", [](std::uintptr_t ptr, int gain) {
        return reinterpret_cast<MainWindow *>(ptr)->mcpSetGainJson(gain);
    });
    m.def("mcp_set_auto_exposure_ptr", [](std::uintptr_t ptr, bool enabled) {
        return reinterpret_cast<MainWindow *>(ptr)->mcpSetAutoExposureJson(enabled);
    });
    m.def("mcp_set_yolo_ptr", [](std::uintptr_t ptr, bool enabled) {
        return reinterpret_cast<MainWindow *>(ptr)->mcpSetYoloJson(enabled);
    });
    m.def("mcp_move_stage_ptr", [](std::uintptr_t ptr, double x, double y, int feedrate) {
        return reinterpret_cast<MainWindow *>(ptr)->mcpMoveStageJson(x, y, feedrate);
    });
    m.def("mcp_emergency_stop_stage_ptr", [](std::uintptr_t ptr) {
        return reinterpret_cast<MainWindow *>(ptr)->mcpEmergencyStopStageJson();
    });
    m.def("mcp_start_scan_ptr", [](std::uintptr_t ptr, double xMin, double yMin,
                                   double xMax, double yMax,
                                   py::object feedrate, py::object serpentine,
                                   py::object homeX, py::object homeY) {
        auto optionalInt = [](const py::object &value) -> std::optional<int> {
            return value.is_none() ? std::nullopt : std::optional<int>{value.cast<int>()};
        };
        auto optionalBool = [](const py::object &value) -> std::optional<bool> {
            return value.is_none() ? std::nullopt : std::optional<bool>{value.cast<bool>()};
        };
        return reinterpret_cast<MainWindow *>(ptr)->mcpStartScanJson(
            xMin, yMin, xMax, yMax, optionalInt(feedrate), optionalBool(serpentine),
            optionalBool(homeX), optionalBool(homeY));
    },
    py::arg("ptr"), py::arg("x_min"), py::arg("y_min"), py::arg("x_max"), py::arg("y_max"),
    py::arg("feedrate") = py::none(), py::arg("serpentine") = py::none(),
    py::arg("home_x") = py::none(), py::arg("home_y") = py::none());
    m.def("mcp_cancel_scan_ptr", [](std::uintptr_t ptr) {
        return reinterpret_cast<MainWindow *>(ptr)->mcpCancelScanJson();
    });
    m.def("mcp_start_autofocus_ptr", [](std::uintptr_t ptr, double range_mm, double step_mm) {
        return reinterpret_cast<MainWindow *>(ptr)->mcpStartAutofocusJson(range_mm, step_mm);
    },
    py::arg("ptr"), py::arg("range_mm") = 0.0, py::arg("step_mm") = 0.0);
    m.def("mcp_cancel_autofocus_ptr", [](std::uintptr_t ptr) {
        return reinterpret_cast<MainWindow *>(ptr)->mcpCancelAutofocusJson();
    });

    // Headless Camera + Stage Server bridge
    m.def("create_camera_server_ptr", [](const std::string &bindAddress,
                                         int port,
                                         int jpegQuality,
                                         const std::string &recordDir,
                                         bool autoStart,
                                         bool keepOpen,
                                         bool synthetic,
                                         const std::string &stagePort,
                                         int stageBaud) {
        auto *server = new MindVisionCameraServer();
        server->setSyntheticMode(synthetic);
        server->setJpegQuality(jpegQuality);
        server->setRecordingDirectory(QString::fromStdString(recordDir));
        server->setKeepCameraOpen(keepOpen);

        QObject::connect(server, &MindVisionCameraServer::logMessage, [](const QString &msg) {
            const QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
            std::cout << "[" << ts.toStdString() << "] " << msg.toStdString() << std::endl;
        });
        QObject::connect(server, &MindVisionCameraServer::errorOccurred, [](const QString &msg) {
            const QString ts = QDateTime::currentDateTime().toString("HH:mm:ss");
            std::cerr << "[" << ts.toStdString() << "] ERROR: " << msg.toStdString() << std::endl;
        });

        const QString qStagePort = QString::fromStdString(stagePort).trimmed();
        if (!qStagePort.isEmpty()) {
            server->connectStage(qStagePort, stageBaud > 0 ? stageBaud : 115200);
        } else if (synthetic && autoStart) {
            server->connectStage(QStringLiteral("SYNTHETIC_STAGE"), stageBaud > 0 ? stageBaud : 115200);
        }

        if (autoStart) {
            server->startCapture();
        }
        if (!server->startServer(QString::fromStdString(bindAddress), static_cast<quint16>(port))) {
            delete server;
            return static_cast<std::uintptr_t>(0);
        }
        return reinterpret_cast<std::uintptr_t>(server);
    },
    py::arg("bind_address") = "0.0.0.0",
    py::arg("port") = 8766,
    py::arg("jpeg_quality") = 85,
    py::arg("record_dir") = "videos",
    py::arg("auto_start") = false,
    py::arg("keep_open") = false,
    py::arg("synthetic") = false,
    py::arg("stage_port") = "",
    py::arg("stage_baud") = 115200);

    m.def("camera_server_port_ptr", [](std::uintptr_t ptr) -> int {
        auto *server = reinterpret_cast<MindVisionCameraServer *>(ptr);
        return server ? static_cast<int>(server->serverPort()) : 0;
    });

    m.def("delete_camera_server_ptr", [](std::uintptr_t ptr) {
        auto *server = reinterpret_cast<MindVisionCameraServer *>(ptr);
        if (server) {
            server->stopServer();
            delete server;
        }
    });

    m.def("camera_server_mcp_status_ptr", [](std::uintptr_t ptr) {
        return reinterpret_cast<MindVisionCameraServer *>(ptr)->mcpStatusJson();
    });
    m.def("camera_server_mcp_start_camera_ptr", [](std::uintptr_t ptr) {
        return reinterpret_cast<MindVisionCameraServer *>(ptr)->mcpStartCameraJson();
    });
    m.def("camera_server_mcp_stop_camera_ptr", [](std::uintptr_t ptr) {
        return reinterpret_cast<MindVisionCameraServer *>(ptr)->mcpStopCameraJson();
    });
    m.def("camera_server_mcp_snapshot_ptr", [](std::uintptr_t ptr) {
        return reinterpret_cast<MindVisionCameraServer *>(ptr)->mcpSnapshotJson();
    });
    m.def("camera_server_mcp_set_recording_ptr", [](std::uintptr_t ptr, bool enabled) {
        return reinterpret_cast<MindVisionCameraServer *>(ptr)->mcpSetRecordingJson(enabled);
    });
    m.def("camera_server_mcp_set_exposure_ptr", [](std::uintptr_t ptr, double milliseconds) {
        return reinterpret_cast<MindVisionCameraServer *>(ptr)->mcpSetExposureJson(milliseconds);
    });
    m.def("camera_server_mcp_set_gain_ptr", [](std::uintptr_t ptr, int gain) {
        return reinterpret_cast<MindVisionCameraServer *>(ptr)->mcpSetGainJson(gain);
    });
    m.def("camera_server_mcp_set_auto_exposure_ptr", [](std::uintptr_t ptr, bool enabled) {
        return reinterpret_cast<MindVisionCameraServer *>(ptr)->mcpSetAutoExposureJson(enabled);
    });
    m.def("camera_server_mcp_move_stage_ptr", [](std::uintptr_t ptr, double x, double y, int feedrate) {
        return reinterpret_cast<MindVisionCameraServer *>(ptr)->mcpMoveStageJson(x, y, feedrate);
    });
    m.def("camera_server_mcp_emergency_stop_stage_ptr", [](std::uintptr_t ptr) {
        return reinterpret_cast<MindVisionCameraServer *>(ptr)->mcpEmergencyStopStageJson();
    });
}
