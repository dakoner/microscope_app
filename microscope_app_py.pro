QT += core gui widgets serialport

TEMPLATE = lib
CONFIG += plugin no_plugin_name_prefix c++20
TARGET = _microscope_app_cpp

SRC_DIR = $$PWD/src
APP_DIR = $$SRC_DIR/microscope_app

QMAKE_LFLAGS += -Wl,--disable-new-dtags

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

# QScintilla configuration
QSCI_INCLUDE = /usr/include/x86_64-linux-gnu/qt6/Qsci
QSCI_LIB = /usr/lib/x86_64-linux-gnu
INCLUDEPATH += $$QSCI_INCLUDE
LIBS += -L$$QSCI_LIB -lqscintilla2_qt6

# MindVision SDK
MVSDK_INCLUDE = $$SRC_DIR/mindvision_qobject/Include
MVSDK_LIB = $$SRC_DIR/mindvision_qobject/Lib
INCLUDEPATH += $$MVSDK_INCLUDE
LIBS += -L$$MVSDK_LIB -lMVSDK

# Project include paths
INCLUDEPATH += \
    $$APP_DIR \
    $$SRC_DIR/mindvision_qobject/src \
    $$SRC_DIR/cnc_control_panel_qobject/src \
    $$SRC_DIR/serial_qobject/src \
    $$SRC_DIR/led_controller_qobject/src \
    $$SRC_DIR/color_picker_widget_qobject/src \
    $$SRC_DIR/intensity_chart_qobject/src \
    $$SRC_DIR/mosaic_panel_qobject/src \
    $$SRC_DIR/editor_qobject/src \
    $$SRC_DIR/scan_config_paneL_qobject/src \
    $$SRC_DIR/yolo_inference_qobject/src

# Python + pybind11 headers
PYTHON_BIN = /home/davidek/.local/share/uv/python/cpython-3.11-linux-x86_64-gnu/bin/python3.11
PYTHON_CONFIG = /home/davidek/.local/share/uv/python/cpython-3.11-linux-x86_64-gnu/bin/python3.11-config
PY_CFLAGS = $$system($$PYTHON_CONFIG --includes)
PY_LDFLAGS = $$system($$PYTHON_CONFIG --embed --ldflags 2>/dev/null)
isEmpty(PY_LDFLAGS): PY_LDFLAGS = $$system($$PYTHON_CONFIG --ldflags)
QMAKE_CXXFLAGS += $$PY_CFLAGS
LIBS += $$PY_LDFLAGS

PYBIND11_INCLUDES = $$system($$PYTHON_BIN -m pybind11 --includes 2>/dev/null)
!isEmpty(PYBIND11_INCLUDES) {
    QMAKE_CXXFLAGS += $$PYBIND11_INCLUDES
}

# TensorRT + CUDA runtime
TENSORRT_INCLUDE = /usr/include/x86_64-linux-gnu
TENSORRT_LIB = /usr/lib/x86_64-linux-gnu
INCLUDEPATH += $$TENSORRT_INCLUDE
LIBS += -L$$TENSORRT_LIB -lnvinfer -lnvinfer_plugin
QMAKE_LFLAGS += -Wl,-rpath,$$TENSORRT_LIB

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

DEFINES += MINDVISION_QOBJECT_LIBRARY

SOURCES += \
    $$APP_DIR/MainWindow.cpp \
    $$APP_DIR/microscope_app_python.cpp \
    $$SRC_DIR/cnc_control_panel_qobject/src/CNCControlPanel.cpp \
    $$SRC_DIR/mosaic_panel_qobject/src/MosaicWidget.cpp \
    $$SRC_DIR/mosaic_panel_qobject/src/MosaicPanel.cpp \
    $$SRC_DIR/intensity_chart_qobject/src/IntensityChart.cpp \
    $$SRC_DIR/color_picker_widget_qobject/src/ColorPickerWidget.cpp \
    $$SRC_DIR/led_controller_qobject/src/LEDController.cpp \
    $$SRC_DIR/scan_config_paneL_qobject/src/ScanConfigPanel.cpp \
    $$SRC_DIR/yolo_inference_qobject/src/YOLOInferenceWorker.cpp \
    $$SRC_DIR/editor_qobject/src/PythonScintillaEditor.cpp \
    $$SRC_DIR/mindvision_qobject/src/MindVisionCamera.cpp \
    $$SRC_DIR/mindvision_qobject/src/VideoThread.cpp \
    $$SRC_DIR/serial_qobject/src/SerialWorker.cpp

HEADERS += \
    $$APP_DIR/MainWindow.h \
    $$SRC_DIR/cnc_control_panel_qobject/src/CNCControlPanel.h \
    $$SRC_DIR/mosaic_panel_qobject/src/MosaicWidget.h \
    $$SRC_DIR/mosaic_panel_qobject/src/MosaicPanel.h \
    $$SRC_DIR/intensity_chart_qobject/src/IntensityChart.h \
    $$SRC_DIR/color_picker_widget_qobject/src/ColorPickerWidget.h \
    $$SRC_DIR/led_controller_qobject/src/LEDController.h \
    $$SRC_DIR/scan_config_paneL_qobject/src/ScanConfigPanel.h \
    $$SRC_DIR/yolo_inference_qobject/src/YOLOInferenceWorker.h \
    $$SRC_DIR/editor_qobject/src/PythonScintillaEditor.h \
    $$SRC_DIR/mindvision_qobject/src/MindVisionCamera.h \
    $$SRC_DIR/mindvision_qobject/src/VideoThread.h \
    $$SRC_DIR/mindvision_qobject/src/mindvision_qobject_global.h \
    $$SRC_DIR/serial_qobject/src/SerialWorker.h

FORMS += \
    $$APP_DIR/MainWindow.ui
