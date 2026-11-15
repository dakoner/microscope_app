QT += core gui widgets serialport network

TARGET = cnc_control_panel_qobject
TEMPLATE = lib
CONFIG += shared c++20

INCLUDEPATH += src \
               ../serial_qobject/src \
               ../mindvision_qobject/src

SOURCES += \
    src/CNCControlPanel.cpp

HEADERS += \
    src/CNCControlPanel.h
