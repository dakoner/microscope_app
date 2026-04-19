QT += core gui widgets

TARGET = color_picker_widget_qobject
TEMPLATE = lib
CONFIG += shared c++20

INCLUDEPATH += src

SOURCES += \
    src/ColorPickerWidget.cpp

HEADERS += \
    src/ColorPickerWidget.h
