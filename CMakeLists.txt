cmake_minimum_required(VERSION 3.16)
project(microscope_app LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets SerialPort)

# MindVision SDK
set(MVSDK_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../mindvision_qobject/Include")
set(MVSDK_LIB_DIR "${CMAKE_SOURCE_DIR}/../mindvision_qobject/Lib")

# mindvision_qobject library sources (built as part of this project)
set(MINDVISION_SOURCES
    ${CMAKE_SOURCE_DIR}/../mindvision_qobject/src/MindVisionCamera.cpp
    ${CMAKE_SOURCE_DIR}/../mindvision_qobject/src/VideoThread.cpp
)
set(MINDVISION_HEADERS
    ${CMAKE_SOURCE_DIR}/../mindvision_qobject/src/MindVisionCamera.h
    ${CMAKE_SOURCE_DIR}/../mindvision_qobject/src/VideoThread.h
    ${CMAKE_SOURCE_DIR}/../mindvision_qobject/src/mindvision_qobject_global.h
)

# serial_qobject library sources
set(SERIAL_SOURCES
    ${CMAKE_SOURCE_DIR}/../serial_qobject/src/SerialWorker.cpp
)
set(SERIAL_HEADERS
    ${CMAKE_SOURCE_DIR}/../serial_qobject/src/SerialWorker.h
)

add_executable(microscope_app
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/MainWindow.ui
    src/CNCControlPanel.cpp
    src/CNCControlPanel.h
    src/MosaicWidget.cpp
    src/MosaicWidget.h
    src/MosaicPanel.cpp
    src/MosaicPanel.h
    src/IntensityChart.cpp
    src/IntensityChart.h
    src/ColorPickerWidget.cpp
    src/ColorPickerWidget.h
    src/LEDController.cpp
    src/LEDController.h
    src/ScanConfigPanel.cpp
    src/ScanConfigPanel.h
    ${MINDVISION_SOURCES}
    ${MINDVISION_HEADERS}
    ${SERIAL_SOURCES}
    ${SERIAL_HEADERS}
)

# Treat mindvision_qobject as exporting symbols (we compile it directly)
target_compile_definitions(microscope_app PRIVATE MINDVISION_QOBJECT_LIBRARY)

target_include_directories(microscope_app PRIVATE
    src
    ${MVSDK_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/../mindvision_qobject/src
    ${CMAKE_SOURCE_DIR}/../serial_qobject/src
)

target_link_directories(microscope_app PRIVATE ${MVSDK_LIB_DIR})

target_link_libraries(microscope_app PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::SerialPort
    MVSDK
)
