QT += core gui widgets

TARGET = intensity_chart_qobject
TEMPLATE = lib
CONFIG += shared c++20

INCLUDEPATH += src

SOURCES += \
    src/IntensityChart.cpp

HEADERS += \
    src/IntensityChart.h
