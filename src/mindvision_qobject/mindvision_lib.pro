QT       += core gui network

TARGET = mindvision_qobject
TEMPLATE = lib
CONFIG += shared

DEFINES += QT_DEPRECATED_WARNINGS
DEFINES += MINDVISION_QOBJECT_LIBRARY

INCLUDEPATH += Include
INCLUDEPATH += src

CONFIG(debug, debug|release) {
    DESTDIR = $$OUT_PWD/debug
    OBJECTS_DIR = $$OUT_PWD/debug
    MOC_DIR = $$OUT_PWD/debug
} else {
    DESTDIR = $$OUT_PWD/release
    OBJECTS_DIR = $$OUT_PWD/release
    MOC_DIR = $$OUT_PWD/release
}

CONFIG += c++2a

SOURCES += \
    src/MindVisionCamera.cpp \
    src/MindVisionCameraServer.cpp \
    src/VideoThread.cpp

HEADERS += \
    src/CameraNetworkProtocol.h \
    src/MindVisionCamera.h \
    src/MindVisionCameraServer.h \
    src/VideoThread.h \
    src/mindvision_qobject_global.h

LIBS += -L$$PWD/Lib -lMVSDK
PYSIDE_QT_LIB = $$PWD/../../.venv/lib/python3.11/site-packages/PySide6/Qt/lib
!exists($$PYSIDE_QT_LIB/libQt6Core.so.6): PYSIDE_QT_LIB = $$PWD/../../.venv/lib/python3.12/site-packages/PySide6/Qt/lib
unix:!macx {
    QMAKE_LFLAGS += -Wl,--disable-new-dtags -Wl,-rpath,$$PWD/Lib
    exists($$PYSIDE_QT_LIB/libQt6Core.so.6) {
        QMAKE_LFLAGS += -Wl,-rpath,$$PYSIDE_QT_LIB
    }
}
