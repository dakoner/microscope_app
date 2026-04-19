QT += core gui widgets serialport

TARGET = microscope_app_dir
TEMPLATE = app
CONFIG += c++20

INCLUDEPATH += . \
               ../cnc_control_panel_qobject/src \
               ../serial_qobject/src \
               ../led_controller_qobject/src \
               ../color_picker_widget_qobject/src \
               ../intensity_chart_qobject/src \
               ../mosaic_panel_qobject/src \
               ../scan_config_paneL_qobject/src \
               ../editor_qobject/src \
               ../yolo_inference_qobject/src \
               ../mindvision_qobject/src \
               ../mindvision_qobject/Include \
               /usr/include/x86_64-linux-gnu/qt6/Qsci

SOURCES += \
    main.cpp \
    MainWindow.cpp

HEADERS += \
    MainWindow.h

FORMS += \
    MainWindow.ui

LIBS += -lqscintilla2_qt6
