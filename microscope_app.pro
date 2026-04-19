QT += core gui widgets serialport

CONFIG += c++20
CONFIG -= debug_and_release
QMAKE_LFLAGS += -Wl,--disable-new-dtags

TARGET = microscope_app
TEMPLATE = app

# QScintilla configuration
QSCI_INCLUDE = /usr/include/x86_64-linux-gnu/qt6/Qsci
QSCI_LIB = /usr/lib/x86_64-linux-gnu
INCLUDEPATH += $$QSCI_INCLUDE
LIBS += -L$$QSCI_LIB -lqscintilla2_qt6

# Build output directories
CONFIG(debug, debug|release) {
    DESTDIR = ./debug
    OBJECTS_DIR = ./debug/.obj
    MOC_DIR = ./debug/.moc
    UI_DIR = ./debug/.ui
    RCC_DIR = ./debug/.rcc
} else {
    DESTDIR = ./release
    OBJECTS_DIR = ./release/.obj
    MOC_DIR = ./release/.moc
    UI_DIR = ./release/.ui
    RCC_DIR = ./release/.rcc
}

# MindVision SDK
MVSDK_INCLUDE = src/mindvision_qobject/Include
MVSDK_LIB = src/mindvision_qobject/Lib

# Include paths
INCLUDEPATH += \
    src/microscope_app \
    $$MVSDK_INCLUDE \
    src/mindvision_qobject/src \
    src/cnc_control_panel_qobject/src \
    src/serial_qobject/src \
    src/led_controller_qobject/src \
    src/color_picker_widget_qobject/src \
    src/intensity_chart_qobject/src \
    src/mosaic_panel_qobject/src \
    src/editor_qobject/src \
    src/scan_config_paneL_qobject/src \
    src/yolo_inference_qobject/src

# Library paths
LIBS += -L$$MVSDK_LIB -lMVSDK

# Embedded Python (CPython C API)
PYTHON_CONFIG = /home/davidek/.local/share/uv/python/cpython-3.11-linux-x86_64-gnu/bin/python3.11-config
PYTHON_BIN = /home/davidek/.local/share/uv/python/cpython-3.11-linux-x86_64-gnu/bin/python3.11
PY_LIBDIR = /home/davidek/.local/share/uv/python/cpython-3.11.15-linux-x86_64-gnu/lib
PY_CFLAGS = $$system($$PYTHON_CONFIG --includes)
PY_LDFLAGS = $$system($$PYTHON_CONFIG --embed --ldflags 2>/dev/null)
isEmpty(PY_LDFLAGS): PY_LDFLAGS = $$system($$PYTHON_CONFIG --ldflags)
QMAKE_CXXFLAGS += $$PY_CFLAGS
LIBS += $$PY_LDFLAGS
!isEmpty(PY_LIBDIR): QMAKE_LFLAGS += -Wl,-rpath,$$PY_LIBDIR

# TensorRT runtime (engine inference backend)
TENSORRT_INCLUDE = /usr/include/x86_64-linux-gnu
TENSORRT_LIB = /usr/lib/x86_64-linux-gnu
INCLUDEPATH += $$TENSORRT_INCLUDE
LIBS += -L$$TENSORRT_LIB -lnvinfer -lnvinfer_plugin
QMAKE_LFLAGS += -Wl,-rpath,$$TENSORRT_LIB

# CUDA toolkit headers/libraries required by TensorRT headers/runtime.
CUDA_INCLUDE = /usr/local/cuda/targets/x86_64-linux/include
CUDA_LIB = /usr/local/cuda/targets/x86_64-linux/lib

!exists($$CUDA_INCLUDE/cuda_runtime_api.h): CUDA_INCLUDE = /usr/local/cuda-13.2/targets/x86_64-linux/include
!exists($$CUDA_LIB/libcudart.so): CUDA_LIB = /usr/local/cuda-13.2/targets/x86_64-linux/lib

!exists($$CUDA_INCLUDE/cuda_runtime_api.h): CUDA_INCLUDE = /usr/local/cuda-13.1/targets/x86_64-linux/include
!exists($$CUDA_LIB/libcudart.so): CUDA_LIB = /usr/local/cuda-13.1/targets/x86_64-linux/lib

exists($$CUDA_INCLUDE/cuda_runtime_api.h) {
    INCLUDEPATH += $$CUDA_INCLUDE
}

exists($$CUDA_LIB/libcudart.so) {
    LIBS += -L$$CUDA_LIB -lcudart
    QMAKE_LFLAGS += -Wl,-rpath,$$CUDA_LIB
} else {
    LIBS += -lcudart
}

# Preprocessor definitions
DEFINES += MINDVISION_QOBJECT_LIBRARY

# Main application sources
SOURCES += \
    src/microscope_app/main.cpp \
    src/microscope_app/MainWindow.cpp \
    src/cnc_control_panel_qobject/src/CNCControlPanel.cpp \
    src/mosaic_panel_qobject/src/MosaicWidget.cpp \
    src/mosaic_panel_qobject/src/MosaicPanel.cpp \
    src/intensity_chart_qobject/src/IntensityChart.cpp \
    src/color_picker_widget_qobject/src/ColorPickerWidget.cpp \
    src/led_controller_qobject/src/LEDController.cpp \
    src/scan_config_paneL_qobject/src/ScanConfigPanel.cpp \
    src/yolo_inference_qobject/src/YOLOInferenceWorker.cpp \
    src/editor_qobject/src/PythonScintillaEditor.cpp \
    src/mindvision_qobject/src/MindVisionCamera.cpp \
    src/mindvision_qobject/src/VideoThread.cpp \
    src/serial_qobject/src/SerialWorker.cpp

HEADERS += \
    src/microscope_app/MainWindow.h \
    src/cnc_control_panel_qobject/src/CNCControlPanel.h \
    src/mosaic_panel_qobject/src/MosaicWidget.h \
    src/mosaic_panel_qobject/src/MosaicPanel.h \
    src/intensity_chart_qobject/src/IntensityChart.h \
    src/color_picker_widget_qobject/src/ColorPickerWidget.h \
    src/led_controller_qobject/src/LEDController.h \
    src/scan_config_paneL_qobject/src/ScanConfigPanel.h \
    src/yolo_inference_qobject/src/YOLOInferenceWorker.h \
    src/editor_qobject/src/PythonScintillaEditor.h \
    src/mindvision_qobject/src/MindVisionCamera.h \
    src/mindvision_qobject/src/VideoThread.h \
    src/mindvision_qobject/src/mindvision_qobject_global.h \
    src/serial_qobject/src/SerialWorker.h

FORMS += \
    src/microscope_app/MainWindow.ui
