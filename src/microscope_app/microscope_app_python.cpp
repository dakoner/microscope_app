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
}
