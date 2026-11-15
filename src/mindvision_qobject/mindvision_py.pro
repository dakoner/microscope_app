QT       += core gui network

TARGET = _mindvision_qobject_py
TEMPLATE = lib
CONFIG += shared

DEFINES += QT_DEPRECATED_WARNINGS

INCLUDEPATH += Include
INCLUDEPATH += src

# Python and pybind11 include paths
PYTHON_BIN = $$PWD/../../.venv/bin/python
!exists($$PYTHON_BIN): PYTHON_BIN = $$system(command -v python3)
isEmpty(PYTHON_BIN): PYTHON_BIN = python3
PYTHON_REAL = $$system(readlink -f $$PYTHON_BIN)
PYTHON_LIBDIR = $$replace(PYTHON_REAL, /bin/[^/]+$, /lib)
PYTHON_CONFIG = $$PYTHON_REAL-config
!exists($$PYTHON_CONFIG): PYTHON_CONFIG = $$system(command -v python3-config)

PYTHON_INCLUDES = $$system($$PYTHON_CONFIG --includes 2>/dev/null)
isEmpty(PYTHON_INCLUDES): PYTHON_INCLUDES = $$system($$PYTHON_BIN -c "import sysconfig; print('-I' + sysconfig.get_paths()['include'])")
QMAKE_CXXFLAGS += $$PYTHON_INCLUDES

PYBIND11_INCLUDES = $$system($$PYTHON_BIN -m pybind11 --includes 2>/dev/null)
isEmpty(PYBIND11_INCLUDES) {
    INCLUDEPATH += /usr/include/pybind11
} else {
    QMAKE_CXXFLAGS += $$PYBIND11_INCLUDES
}

CONFIG += c++17

# Directory where the Python module will be placed
CONFIG(debug, debug|release) {
    DESTDIR = $$OUT_PWD/debug
    OBJECTS_DIR = $$OUT_PWD/debug
    MOC_DIR = $$OUT_PWD/debug
} else {
    DESTDIR = $$OUT_PWD/release
    OBJECTS_DIR = $$OUT_PWD/release
    MOC_DIR = $$OUT_PWD/release
}

SOURCES += src/mindvision_qobject_python.cpp

# Link against the main C++ library
CONFIG(debug, debug|release) {
    LIBS += -L$$PWD/debug
} else {
    LIBS += -L$$PWD/release
}
LIBS += -lmindvision_qobject

# Link against the MindVision SDK
LIBS += -L$$PWD/Lib -lMVSDK

# Link against the Python library
win32: {
    PYTHON_LIB_DIR = C:/Users/davidek/scoop/apps/python313/current/libs
    LIBS += -L$$PYTHON_LIB_DIR -lpython3.11
} else: {
    PYTHON_LDFLAGS = $$system($$PYTHON_CONFIG --embed --ldflags 2>/dev/null)
    isEmpty(PYTHON_LDFLAGS): PYTHON_LDFLAGS = $$system($$PYTHON_CONFIG --ldflags 2>/dev/null)
    !isEmpty(PYTHON_LDFLAGS) {
        LIBS += $$PYTHON_LDFLAGS
    }
    !isEmpty(PYTHON_LIBDIR) {
        QMAKE_RPATHDIR += $$PYTHON_LIBDIR
    }
    PYSIDE_QT_LIB = $$PWD/../../.venv/lib/python3.11/site-packages/PySide6/Qt/lib
    !exists($$PYSIDE_QT_LIB/libQt6Core.so.6): PYSIDE_QT_LIB = $$PWD/../../.venv/lib/python3.12/site-packages/PySide6/Qt/lib
    QMAKE_LFLAGS += -Wl,--disable-new-dtags
    exists($$PYSIDE_QT_LIB/libQt6Core.so.6) {
        QMAKE_LFLAGS += -Wl,-rpath,$$PYSIDE_QT_LIB
    }
    QMAKE_RPATHDIR += $$OUT_PWD/release $$PWD/Lib
}

unix:!macx: {
    QMAKE_POST_LINK += $$QMAKE_MOVE $$shell_path($$DESTDIR/lib_mindvision_qobject_py.so) $$shell_path($$DESTDIR/_mindvision_qobject_py.so)
}
