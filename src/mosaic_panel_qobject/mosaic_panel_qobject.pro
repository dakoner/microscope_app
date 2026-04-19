QT += core gui widgets

TARGET = mosaic_panel_qobject
TEMPLATE = lib
CONFIG += shared c++20

INCLUDEPATH += src

SOURCES += \
    src/MosaicPanel.cpp \
    src/MosaicWidget.cpp

HEADERS += \
    src/MosaicPanel.h \
    src/MosaicWidget.h
