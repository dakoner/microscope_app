#include <pybind11/pybind11.h>

#include "MainWindow.h"

#include <cstdint>

namespace py = pybind11;

PYBIND11_MODULE(_microscope_app_cpp, m)
{
    m.doc() = "Pybind11 bridge for C++ microscope MainWindow";

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
}
