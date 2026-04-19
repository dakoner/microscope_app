QT += core gui widgets serialport

TARGET = led_controller_qobject
TEMPLATE = lib
CONFIG += shared c++20

INCLUDEPATH += src \
               ../serial_qobject/src

SOURCES += \
    src/LEDController.cpp

HEADERS += \
    src/LEDController.h
