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
MVSDK_INCLUDE = ../mindvision_qobject/Include
MVSDK_LIB = ../mindvision_qobject/Lib

# Include paths
INCLUDEPATH += \
    src \
    $$MVSDK_INCLUDE \
    ../mindvision_qobject/src \
    ../serial_qobject/src

# Library paths
LIBS += -L$$MVSDK_LIB -lMVSDK

# Preprocessor definitions
DEFINES += MINDVISION_QOBJECT_LIBRARY

# Main application sources
SOURCES += \
    src/main.cpp \
    src/MainWindow.cpp \
    src/CNCControlPanel.cpp \
    src/MosaicWidget.cpp \
    src/MosaicPanel.cpp \
    src/IntensityChart.cpp \
    src/ColorPickerWidget.cpp \
    src/LEDController.cpp \
    src/ScanConfigPanel.cpp \
    ../mindvision_qobject/src/MindVisionCamera.cpp \
    ../mindvision_qobject/src/VideoThread.cpp \
    ../serial_qobject/src/SerialWorker.cpp

HEADERS += \
    src/MainWindow.h \
    src/CNCControlPanel.h \
    src/MosaicWidget.h \
    src/MosaicPanel.h \
    src/IntensityChart.h \
    src/ColorPickerWidget.h \
    src/LEDController.h \
    src/ScanConfigPanel.h \
    ../mindvision_qobject/src/MindVisionCamera.h \
    ../mindvision_qobject/src/VideoThread.h \
    ../mindvision_qobject/src/mindvision_qobject_global.h \
    ../serial_qobject/src/SerialWorker.h

FORMS += \
    src/MainWindow.ui
