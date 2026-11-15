QT       += core gui network serialport

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
unix:!macx {
    QMAKE_LFLAGS += -Wl,--disable-new-dtags -Wl,-rpath,$$PWD/Lib
}
