QT       += core gui

TARGET = yolo_inference_qobject
TEMPLATE = lib
CONFIG += shared c++20

DEFINES += QT_DEPRECATED_WARNINGS

QMAKE_LFLAGS += -Wl,--disable-new-dtags

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

SOURCES += \
    src/YOLOInferenceWorker.cpp

HEADERS += \
    src/YOLOInferenceWorker.h

# TensorRT runtime (engine inference backend)
TENSORRT_INCLUDE = /usr/include/x86_64-linux-gnu
TENSORRT_LIB = /usr/lib/x86_64-linux-gnu
INCLUDEPATH += $$TENSORRT_INCLUDE
LIBS += -L$$TENSORRT_LIB -lnvinfer -lnvinfer_plugin
QMAKE_LFLAGS += -Wl,-rpath,$$TENSORRT_LIB

# CUDA toolkit headers/libraries required by TensorRT headers/runtime.
CUDA_INCLUDE = /usr/local/cuda/targets/x86_64-linux/include
CUDA_LIB = /usr/local/cuda/targets/x86_64-linux/lib

!exists($$CUDA_INCLUDE/cuda_runtime_api.h): CUDA_INCLUDE = /usr/local/cuda-13.2/targets/x86_64-linux/include
!exists($$CUDA_LIB/libcudart.so): CUDA_LIB = /usr/local/cuda-13.2/targets/x86_64-linux/lib

!exists($$CUDA_INCLUDE/cuda_runtime_api.h): CUDA_INCLUDE = /usr/local/cuda-13.1/targets/x86_64-linux/include
!exists($$CUDA_LIB/libcudart.so): CUDA_LIB = /usr/local/cuda-13.1/targets/x86_64-linux/lib

exists($$CUDA_INCLUDE/cuda_runtime_api.h) {
    INCLUDEPATH += $$CUDA_INCLUDE
}

exists($$CUDA_LIB/libcudart.so) {
    LIBS += -L$$CUDA_LIB -lcudart
    QMAKE_LFLAGS += -Wl,-rpath,$$CUDA_LIB
} else {
    LIBS += -lcudart
}
