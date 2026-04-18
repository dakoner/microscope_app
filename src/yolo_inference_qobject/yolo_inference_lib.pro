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

# LibTorch (TorchScript inference backend)
LIBTORCH_ROOT = /home/davidek/src/libtorch
LIBTORCH_INCLUDE = $$LIBTORCH_ROOT/include
LIBTORCH_API_INCLUDE = $$LIBTORCH_ROOT/include/torch/csrc/api/include
LIBTORCH_LIB = $$LIBTORCH_ROOT/lib
INCLUDEPATH += $$LIBTORCH_INCLUDE $$LIBTORCH_API_INCLUDE
LIBS += -L$$LIBTORCH_LIB -ltorch -ltorch_cpu -lc10

TORCH_CUDA_LIB = $$files($$LIBTORCH_LIB/libtorch_cuda.so)
!isEmpty(TORCH_CUDA_LIB) {
    CUDA_HAS_CUDNN = $$system(ldconfig -p | grep -c 'libcudnn.so')
    CUDA_HAS_NCCL = $$system(ldconfig -p | grep -c 'libnccl.so')

    CUDA_CUDNN_DIR = /home/davidek/src/microtools/microscope_app/.venv/lib/python3.12/site-packages/nvidia/cudnn/lib
    CUDA_NCCL_DIR = /home/davidek/src/microtools/microscope_app/.venv/lib/python3.12/site-packages/nvidia/nccl/lib
    CUDA_CUSPARSELT_DIR = /home/davidek/src/microtools/microscope_app/.venv/lib/python3.12/site-packages/nvidia/cusparselt/lib
    CUDA_NVSHMEM_DIR = /home/davidek/src/microtools/microscope_app/.venv/lib/python3.12/site-packages/nvidia/nvshmem/lib

    !exists($$CUDA_CUDNN_DIR/libcudnn.so.9): CUDA_CUDNN_DIR = /home/davidek/.venv/lib/python3.12/site-packages/nvidia/cudnn/lib
    !exists($$CUDA_NCCL_DIR/libnccl.so.2): CUDA_NCCL_DIR = /home/davidek/.venv/lib/python3.12/site-packages/nvidia/nccl/lib
    !exists($$CUDA_CUSPARSELT_DIR/libcusparseLt.so.0): CUDA_CUSPARSELT_DIR = /home/davidek/.venv/lib/python3.12/site-packages/nvidia/cusparselt/lib
    !exists($$CUDA_NVSHMEM_DIR/libnvshmem_host.so.3): CUDA_NVSHMEM_DIR = /home/davidek/.venv/lib/python3.12/site-packages/nvidia/nvshmem/lib

    !exists($$CUDA_CUDNN_DIR/libcudnn.so.9): CUDA_CUDNN_DIR =
    !exists($$CUDA_NCCL_DIR/libnccl.so.2): CUDA_NCCL_DIR =
    !exists($$CUDA_CUSPARSELT_DIR/libcusparseLt.so.0): CUDA_CUSPARSELT_DIR =
    !exists($$CUDA_NVSHMEM_DIR/libnvshmem_host.so.3): CUDA_NVSHMEM_DIR =

    greaterThan(CUDA_HAS_CUDNN, 0)|!isEmpty(CUDA_CUDNN_DIR) {
        greaterThan(CUDA_HAS_NCCL, 0)|!isEmpty(CUDA_NCCL_DIR) {
            LIBS += -ltorch_cuda -lc10_cuda
            DEFINES += HAS_TORCH_CUDA
            QMAKE_LFLAGS += -Wl,-rpath-link,$$LIBTORCH_LIB
            QMAKE_LFLAGS += -Wl,--allow-shlib-undefined
            QMAKE_LFLAGS += -Wl,--no-as-needed

            !isEmpty(CUDA_CUDNN_DIR) {
                LIBS += -L$$CUDA_CUDNN_DIR $$CUDA_CUDNN_DIR/libcudnn.so.9
                QMAKE_LFLAGS += -Wl,-rpath,$$CUDA_CUDNN_DIR -Wl,-rpath-link,$$CUDA_CUDNN_DIR
            }
            !isEmpty(CUDA_NCCL_DIR) {
                LIBS += -L$$CUDA_NCCL_DIR $$CUDA_NCCL_DIR/libnccl.so.2
                QMAKE_LFLAGS += -Wl,-rpath,$$CUDA_NCCL_DIR -Wl,-rpath-link,$$CUDA_NCCL_DIR
            }
            !isEmpty(CUDA_CUSPARSELT_DIR) {
                LIBS += -L$$CUDA_CUSPARSELT_DIR $$CUDA_CUSPARSELT_DIR/libcusparseLt.so.0
                QMAKE_LFLAGS += -Wl,-rpath,$$CUDA_CUSPARSELT_DIR -Wl,-rpath-link,$$CUDA_CUSPARSELT_DIR
            }
            !isEmpty(CUDA_NVSHMEM_DIR) {
                LIBS += -L$$CUDA_NVSHMEM_DIR $$CUDA_NVSHMEM_DIR/libnvshmem_host.so.3
                QMAKE_LFLAGS += -Wl,-rpath,$$CUDA_NVSHMEM_DIR -Wl,-rpath-link,$$CUDA_NVSHMEM_DIR
            }
            QMAKE_LFLAGS += -Wl,--as-needed
        } else {
            message("LibTorch CUDA detected but NCCL missing; building CPU fallback.")
        }
    } else {
        message("LibTorch CUDA detected but cuDNN missing; building CPU fallback.")
    }
}

QMAKE_LFLAGS += -Wl,-rpath,$$LIBTORCH_LIB
