QT += core gui widgets

TARGET = editor_qobject
TEMPLATE = lib
CONFIG += shared c++20

INCLUDEPATH += src \
               /usr/include/x86_64-linux-gnu/qt6/Qsci

SOURCES += \
    src/PythonConsoleWidget.cpp \
    src/PythonScintillaEditor.cpp

HEADERS += \
    src/PythonConsoleWidget.h \
    src/PythonScintillaEditor.h

LIBS += -lqscintilla2_qt6
