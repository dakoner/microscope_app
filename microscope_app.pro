QT += core gui widgets serialport

CONFIG += c++20
CONFIG -= debug_and_release

TARGET = microscope_app
TEMPLATE = app

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
    src/serial_qobject/src

# Library paths
LIBS += -L$$MVSDK_LIB -lMVSDK

# Preprocessor definitions
DEFINES += MINDVISION_QOBJECT_LIBRARY

# Main application sources
SOURCES += \
    src/microscope_app/main.cpp \
    src/microscope_app/MainWindow.cpp \
    src/microscope_app/CNCControlPanel.cpp \
    src/microscope_app/MosaicWidget.cpp \
    src/microscope_app/MosaicPanel.cpp \
    src/microscope_app/IntensityChart.cpp \
    src/microscope_app/ColorPickerWidget.cpp \
    src/microscope_app/LEDController.cpp \
    src/microscope_app/ScanConfigPanel.cpp \
    src/mindvision_qobject/src/MindVisionCamera.cpp \
    src/mindvision_qobject/src/VideoThread.cpp \
    src/serial_qobject/src/SerialWorker.cpp

HEADERS += \
    src/microscope_app/MainWindow.h \
    src/microscope_app/CNCControlPanel.h \
    src/microscope_app/MosaicWidget.h \
    src/microscope_app/MosaicPanel.h \
    src/microscope_app/IntensityChart.h \
    src/microscope_app/ColorPickerWidget.h \
    src/microscope_app/LEDController.h \
    src/microscope_app/ScanConfigPanel.h \
    src/mindvision_qobject/src/MindVisionCamera.h \
    src/mindvision_qobject/src/VideoThread.h \
    src/mindvision_qobject/src/mindvision_qobject_global.h \
    src/serial_qobject/src/SerialWorker.h

FORMS += \
    src/microscope_app/MainWindow.ui
