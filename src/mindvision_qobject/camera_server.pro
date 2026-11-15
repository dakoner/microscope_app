QT += core gui network serialport

TARGET = camera_server
TEMPLATE = app

DEFINES += QT_DEPRECATED_WARNINGS

INCLUDEPATH += Include
INCLUDEPATH += src

CONFIG += c++2a

CONFIG(debug, debug|release) {
    DESTDIR = $$OUT_PWD/debug
    OBJECTS_DIR = $$OUT_PWD/debug/server
    MOC_DIR = $$OUT_PWD/debug/server
} else {
    DESTDIR = $$OUT_PWD/release
    OBJECTS_DIR = $$OUT_PWD/release/server
    MOC_DIR = $$OUT_PWD/release/server
}

SOURCES += \
    src/camera_server_main.cpp

CONFIG(debug, debug|release) {
    LIBS += -L$$PWD/debug
    QMAKE_RPATHDIR += $$OUT_PWD/debug
} else {
    LIBS += -L$$PWD/release
    QMAKE_RPATHDIR += $$OUT_PWD/release
}

LIBS += -lmindvision_qobject
LIBS += -L$$PWD/Lib -lMVSDK

unix:!macx {
    QMAKE_LFLAGS += -Wl,--disable-new-dtags
    QMAKE_RPATHDIR += $$PWD/Lib
}
