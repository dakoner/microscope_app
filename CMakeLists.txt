# microscope_app - C++/Qt6 sources exposed to Python through pybind11.
#
# Linux keeps using microscope_app.pro (qmake).  This file is the build used on
# Windows, where the module is produced as release/_microscope_app_cpp.pyd and
# loaded by run_microscope_app.py, exactly like the qmake build on Linux.
#
# Configure (from a Visual Studio developer shell, see scripts/build_windows.ps1):
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build

cmake_minimum_required(VERSION 3.22)
project(microscope_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# The Python launcher adds <project>/release to sys.path, so the module always
# lands there regardless of the build directory.
set(MICROSCOPE_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/release")
file(MAKE_DIRECTORY "${MICROSCOPE_OUTPUT_DIR}")

set(SRC_DIR "${CMAKE_SOURCE_DIR}/src")
set(APP_DIR "${SRC_DIR}/microscope_app")

# ---------------------------------------------------------------------------
# Qt 6 (Core/GUI/Widgets/Network) plus Qt SerialPort
# ---------------------------------------------------------------------------
set(THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party")
if(WIN32)
    set(MICROSCOPE_PREFIX "${THIRD_PARTY_DIR}/x64")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

if(WIN32)
    # The local Qt kit has no SerialPort component; scripts/build_deps_windows.ps1
    # builds it from the matching qtserialport sources into third_party/x64.
    find_package(Qt6SerialPort REQUIRED CONFIG
        HINTS "${MICROSCOPE_PREFIX}/lib/cmake/Qt6SerialPort" NO_DEFAULT_PATH)
else()
    find_package(Qt6 REQUIRED COMPONENTS SerialPort)
endif()

# ---------------------------------------------------------------------------
# Python + pybind11 (the venv is expected on CMAKE_PREFIX_PATH)
# ---------------------------------------------------------------------------
find_package(Python 3.11 REQUIRED COMPONENTS Interpreter Development.Module)
find_package(pybind11 CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# QScintilla + libtiff (built by scripts/build_deps_windows.ps1 on Windows)
# ---------------------------------------------------------------------------
if(WIN32)
    if(NOT EXISTS "${MICROSCOPE_PREFIX}/include/Qsci/qsciscintilla.h")
        message(FATAL_ERROR
            "QScintilla/libtiff not found in ${MICROSCOPE_PREFIX}. "
            "Run scripts/build_deps_windows.ps1 first.")
    endif()

    add_library(QScintilla::QScintilla SHARED IMPORTED GLOBAL)
    set_target_properties(QScintilla::QScintilla PROPERTIES
        IMPORTED_IMPLIB "${MICROSCOPE_PREFIX}/lib/qscintilla2_qt6.lib"
        IMPORTED_LOCATION "${MICROSCOPE_PREFIX}/bin/qscintilla2_qt6.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${MICROSCOPE_PREFIX}/include"
        # qsciglobal.h only switches to __declspec(dllimport) when this macro is
        # defined; without it MSVC cannot resolve imported data such as
        # QsciScintilla::staticMetaObject.
        INTERFACE_COMPILE_DEFINITIONS "QSCINTILLA_DLL"
    )

    add_library(TIFF::TIFF SHARED IMPORTED GLOBAL)
    set_target_properties(TIFF::TIFF PROPERTIES
        IMPORTED_IMPLIB "${MICROSCOPE_PREFIX}/lib/tiff.lib"
        IMPORTED_LOCATION "${MICROSCOPE_PREFIX}/bin/tiff.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${MICROSCOPE_PREFIX}/include"
    )
else()
    # Linux: system QScintilla and libtiff, as in microscope_app.pro.
    find_library(QSCINTILLA_LIB qscintilla2_qt6 REQUIRED)
    find_path(QSCINTILLA_INCLUDE_DIR Qsci/qsciscintilla.h REQUIRED)
    add_library(QScintilla::QScintilla UNKNOWN IMPORTED GLOBAL)
    set_target_properties(QScintilla::QScintilla PROPERTIES
        IMPORTED_LOCATION "${QSCINTILLA_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${QSCINTILLA_INCLUDE_DIR}"
    )
    find_package(TIFF REQUIRED)
endif()

# ---------------------------------------------------------------------------
# MindVision SDK
# ---------------------------------------------------------------------------
set(MVSDK_DIR "${SRC_DIR}/mindvision_qobject")
set(MVSDK_INCLUDE "${MVSDK_DIR}/Include")
set(MVSDK_LIB "${MVSDK_DIR}/Lib")
if(WIN32)
    set(MVSDK_IMPLIB "${MVSDK_LIB}/MVCAMSDK_X64.lib")
    if(NOT EXISTS "${MVSDK_IMPLIB}")
        message(FATAL_ERROR "MindVision import library not found: ${MVSDK_IMPLIB}")
    endif()
    set(MVSDK_RUNTIME_DLLS
        "C:/Program Files (x86)/MindVision/SDK/X64/MVCAMSDK_X64.dll"
        "${MVSDK_LIB}/MVCAMSDK_X64.dll")
else()
    set(MVSDK_IMPLIB "${MVSDK_LIB}/libMVSDK.so")
endif()

# ---------------------------------------------------------------------------
# Optional YOLO / TensorRT + CUDA support
# ---------------------------------------------------------------------------
option(MICROSCOPE_ENABLE_YOLO "Build the YOLO/TensorRT object detection worker" OFF)

set(MICROSCOPE_SOURCES
    "${APP_DIR}/MainWindow.cpp"
    "${APP_DIR}/FocusMetric.cpp"
    "${APP_DIR}/WaypointPanel.cpp"
    "${APP_DIR}/microscope_app_python.cpp"
    "${SRC_DIR}/cnc_control_panel_qobject/src/CNCControlPanel.cpp"
    "${SRC_DIR}/mosaic_panel_qobject/src/MosaicWidget.cpp"
    "${SRC_DIR}/mosaic_panel_qobject/src/MosaicPanel.cpp"
    "${SRC_DIR}/intensity_chart_qobject/src/IntensityChart.cpp"
    "${SRC_DIR}/intensity_chart_qobject/src/HistogramWidget.cpp"
    "${SRC_DIR}/color_picker_widget_qobject/src/ColorPickerWidget.cpp"
    "${SRC_DIR}/led_controller_qobject/src/LEDController.cpp"
    "${SRC_DIR}/scan_config_paneL_qobject/src/ScanConfigPanel.cpp"
    "${SRC_DIR}/editor_qobject/src/PythonScintillaEditor.cpp"
    "${SRC_DIR}/mindvision_qobject/src/MindVisionCamera.cpp"
    "${SRC_DIR}/mindvision_qobject/src/MindVisionCameraServer.cpp"
    "${SRC_DIR}/mindvision_qobject/src/VideoThread.cpp"
    "${SRC_DIR}/serial_qobject/src/SerialWorker.cpp"
)

set(MICROSCOPE_HEADERS
    "${APP_DIR}/MainWindow.h"
    "${APP_DIR}/WaypointPanel.h"
    "${SRC_DIR}/cnc_control_panel_qobject/src/CNCControlPanel.h"
    "${SRC_DIR}/mosaic_panel_qobject/src/MosaicWidget.h"
    "${SRC_DIR}/mosaic_panel_qobject/src/MosaicPanel.h"
    "${SRC_DIR}/intensity_chart_qobject/src/IntensityChart.h"
    "${SRC_DIR}/intensity_chart_qobject/src/HistogramWidget.h"
    "${SRC_DIR}/color_picker_widget_qobject/src/ColorPickerWidget.h"
    "${SRC_DIR}/led_controller_qobject/src/LEDController.h"
    "${SRC_DIR}/scan_config_paneL_qobject/src/ScanConfigPanel.h"
    "${SRC_DIR}/editor_qobject/src/PythonScintillaEditor.h"
    "${SRC_DIR}/mindvision_qobject/src/CameraNetworkProtocol.h"
    "${SRC_DIR}/mindvision_qobject/src/MindVisionCamera.h"
    "${SRC_DIR}/mindvision_qobject/src/MindVisionCameraServer.h"
    "${SRC_DIR}/mindvision_qobject/src/VideoThread.h"
    "${SRC_DIR}/mindvision_qobject/src/mindvision_qobject_global.h"
    "${SRC_DIR}/serial_qobject/src/SerialWorker.h"
)

set(MICROSCOPE_INCLUDE_DIRS
    "${APP_DIR}"
    "${MVSDK_INCLUDE}"
    "${SRC_DIR}/mindvision_qobject/src"
    "${SRC_DIR}/cnc_control_panel_qobject/src"
    "${SRC_DIR}/mosaic_panel_qobject/src"
    "${SRC_DIR}/serial_qobject/src"
    "${SRC_DIR}/led_controller_qobject/src"
    "${SRC_DIR}/color_picker_widget_qobject/src"
    "${SRC_DIR}/intensity_chart_qobject/src"
    "${SRC_DIR}/mosaic_panel_qobject/src"
    "${SRC_DIR}/editor_qobject/src"
    "${SRC_DIR}/scan_config_paneL_qobject/src"
)

if(MICROSCOPE_ENABLE_YOLO)
    message(STATUS "Building microscope_app WITH YOLO/TensorRT support")
    list(APPEND MICROSCOPE_SOURCES
        "${SRC_DIR}/yolo_inference_qobject/src/YOLOInferenceWorker.cpp")
    list(APPEND MICROSCOPE_HEADERS
        "${SRC_DIR}/yolo_inference_qobject/src/YOLOInferenceWorker.h")
    list(APPEND MICROSCOPE_INCLUDE_DIRS "${SRC_DIR}/yolo_inference_qobject/src")
else()
    message(STATUS "Building microscope_app WITHOUT YOLO/TensorRT support")
endif()

# ---------------------------------------------------------------------------
# The pybind11 extension module imported by run_microscope_app.py
# ---------------------------------------------------------------------------
pybind11_add_module(_microscope_app_cpp MODULE
    ${MICROSCOPE_SOURCES}
    ${MICROSCOPE_HEADERS}
    "${APP_DIR}/MainWindow.ui"
)

set_target_properties(_microscope_app_cpp PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${MICROSCOPE_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${MICROSCOPE_OUTPUT_DIR}"
)
if(WIN32)
    # Keep the file name stable (_microscope_app_cpp.pyd) instead of the
    # interpreter-tagged .cp312-win_amd64.pyd, mirroring the Linux .so name.
    set_target_properties(_microscope_app_cpp PROPERTIES SUFFIX ".pyd")
endif()

target_include_directories(_microscope_app_cpp PRIVATE ${MICROSCOPE_INCLUDE_DIRS})
target_compile_definitions(_microscope_app_cpp PRIVATE
    MINDVISION_QOBJECT_LIBRARY
    MICROSCOPE_ENABLE_YOLO=$<BOOL:${MICROSCOPE_ENABLE_YOLO}>
    _USE_MATH_DEFINES
    NOMINMAX
)

target_link_libraries(_microscope_app_cpp PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::SerialPort
    QScintilla::QScintilla
    TIFF::TIFF
    ${MVSDK_IMPLIB}
)

if(MICROSCOPE_ENABLE_YOLO)
    if(WIN32)
        # CUDA: the app has no .cu sources, so the CUDART headers and import
        # library from third_party/x64 (see build_deps_windows.ps1) are enough;
        # a full CUDA toolkit install is not required.
        find_path(CUDA_CUDART_INCLUDE_DIR cuda_runtime_api.h
            HINTS "${MICROSCOPE_PREFIX}/include" NO_DEFAULT_PATH REQUIRED)
        find_library(CUDA_CUDART_LIB cudart
            HINTS "${MICROSCOPE_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)

        # TensorRT ships as an unpacked NVIDIA zip under third_party/.  Only the
        # extracted directory counts: the downloaded archive itself also starts
        # with "TensorRT-", and picking it would break the lookups below.
        if(NOT MICROSCOPE_TENSORRT_DIR)
            set(trt_candidates "")
            file(GLOB trt_entries "${THIRD_PARTY_DIR}/TensorRT-*")
            foreach(trt_entry IN LISTS trt_entries)
                if(IS_DIRECTORY "${trt_entry}")
                    list(APPEND trt_candidates "${trt_entry}")
                endif()
            endforeach()
            if(trt_candidates)
                list(SORT trt_candidates COMPARE NATURAL ORDER DESCENDING)
                list(GET trt_candidates 0 MICROSCOPE_TENSORRT_DIR)
            endif()
        endif()
        if(NOT MICROSCOPE_TENSORRT_DIR OR NOT IS_DIRECTORY "${MICROSCOPE_TENSORRT_DIR}")
            message(FATAL_ERROR
                "TensorRT not found. Pass -DMICROSCOPE_TENSORRT_DIR=<path> or run "
                "scripts/build_deps_windows.ps1 -TensorRtZip <nvidia zip>.")
        endif()
        find_path(TENSORRT_INCLUDE_DIR NvInferRuntime.h
            HINTS "${MICROSCOPE_TENSORRT_DIR}/include" NO_DEFAULT_PATH REQUIRED)
        # TensorRT names its import libraries after the major version:
        # nvinfer_11.lib (11.x), nvinfer_10.lib (10.x), nvinfer.lib (8.x/9.x).
        find_library(NVINFER_LIB NAMES nvinfer_11 nvinfer_10 nvinfer
            HINTS "${MICROSCOPE_TENSORRT_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
        find_library(NVINFER_PLUGIN_LIB NAMES nvinfer_plugin_11 nvinfer_plugin_10 nvinfer_plugin
            HINTS "${MICROSCOPE_TENSORRT_DIR}/lib" NO_DEFAULT_PATH REQUIRED)

        # 11.x zips keep the DLLs in bin/, 10.x zips keep them in lib/.
        file(GLOB MICROSCOPE_TRT_DLLS
            "${MICROSCOPE_TENSORRT_DIR}/bin/nvinfer*.dll"
            "${MICROSCOPE_TENSORRT_DIR}/lib/nvinfer*.dll")
        target_include_directories(_microscope_app_cpp PRIVATE
            "${TENSORRT_INCLUDE_DIR}" "${CUDA_CUDART_INCLUDE_DIR}")
        target_link_libraries(_microscope_app_cpp PRIVATE
            "${CUDA_CUDART_LIB}" "${NVINFER_LIB}" "${NVINFER_PLUGIN_LIB}")
        message(STATUS "TensorRT: ${MICROSCOPE_TENSORRT_DIR}")
    else()
        find_package(CUDAToolkit REQUIRED)
        find_library(NVINFER_LIB nvinfer REQUIRED)
        find_library(NVINFER_PLUGIN_LIB nvinfer_plugin REQUIRED)
        target_link_libraries(_microscope_app_cpp PRIVATE
            CUDA::cudart ${NVINFER_LIB} ${NVINFER_PLUGIN_LIB})
    endif()
endif()

if(MSVC)
    # /wd4828: the MindVision SDK headers contain non-UTF8 (GBK) comment bytes.
    target_compile_options(_microscope_app_cpp PRIVATE /utf-8 /wd4996 /wd4091 /wd4828)
else()
    target_compile_options(_microscope_app_cpp PRIVATE -fPIC)
    set_target_properties(_microscope_app_cpp PROPERTIES
        INSTALL_RPATH "${MVSDK_LIB}" BUILD_RPATH "${MVSDK_LIB}")
endif()

# ---------------------------------------------------------------------------
# Copy the native runtime dependencies next to the module
# ---------------------------------------------------------------------------
if(WIN32)
    set(MICROSCOPE_RUNTIME_DLLS
        "${MICROSCOPE_PREFIX}/bin/tiff.dll"
        "${MICROSCOPE_PREFIX}/bin/Qt6SerialPort.dll"
        "${MICROSCOPE_PREFIX}/bin/qscintilla2_qt6.dll"
    )
    if(MICROSCOPE_ENABLE_YOLO)
        # Only CUDART is copied next to the module; the TensorRT DLLs are
        # hundreds of MB, so run_microscope_app.py adds their directory to the
        # DLL search path instead.
        file(GLOB MICROSCOPE_CUDART_DLLS "${MICROSCOPE_PREFIX}/bin/cudart*.dll")
        list(APPEND MICROSCOPE_RUNTIME_DLLS ${MICROSCOPE_CUDART_DLLS})
    endif()
    set(MVSDK_RUNTIME "")
    foreach(candidate IN LISTS MVSDK_RUNTIME_DLLS)
        if(EXISTS "${candidate}")
            set(MVSDK_RUNTIME "${candidate}")
            list(APPEND MICROSCOPE_RUNTIME_DLLS "${candidate}")
            break()
        endif()
    endforeach()
    if(NOT MVSDK_RUNTIME)
        message(WARNING
            "MindVision MVCAMSDK_X64.dll was not found; the module will not load "
            "without it. Install the MindVision SDK or copy the DLL into ${MICROSCOPE_OUTPUT_DIR}.")
    endif()

    add_custom_command(TARGET _microscope_app_cpp POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${MICROSCOPE_RUNTIME_DLLS}
            "${MICROSCOPE_OUTPUT_DIR}/"
        COMMENT "Copying native runtime DLLs into release/"
    )
endif()
