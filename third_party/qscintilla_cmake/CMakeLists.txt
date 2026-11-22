# CMake wrapper around the upstream QScintilla qmake project.
#
# qmake's nmake backend has no parallel build on Windows, so the same source
# list is compiled here with Ninja.  Point QSCI_SRC_DIR at the unpacked
# QScintilla_src-* directory.

cmake_minimum_required(VERSION 3.16)
project(qscintilla LANGUAGES CXX)

if(NOT DEFINED QSCI_SRC_DIR)
    message(FATAL_ERROR "QSCI_SRC_DIR must point at the QScintilla_src-* directory")
endif()
file(TO_CMAKE_PATH "${QSCI_SRC_DIR}" QSCI_SRC_DIR)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets PrintSupport)

set(QSCI_DIR "${QSCI_SRC_DIR}/src")
set(SCINTILLA_DIR "${QSCI_SRC_DIR}/scintilla")

file(GLOB QSCI_WRAPPER_SOURCES "${QSCI_DIR}/*.cpp")
file(GLOB QSCI_WRAPPER_HEADERS "${QSCI_DIR}/*.h")
file(GLOB QSCI_PUBLIC_HEADERS "${QSCI_DIR}/Qsci/*.h")
file(GLOB SCINTILLA_SOURCES "${SCINTILLA_DIR}/src/*.cpp")
file(GLOB SCINTILLA_LEXLIB_SOURCES "${SCINTILLA_DIR}/lexlib/*.cpp")
file(GLOB SCINTILLA_LEXERS_SOURCES "${SCINTILLA_DIR}/lexers/*.cpp")

add_library(qscintilla2_qt6 SHARED
    ${QSCI_WRAPPER_SOURCES}
    ${QSCI_WRAPPER_HEADERS}
    ${QSCI_PUBLIC_HEADERS}
    ${SCINTILLA_SOURCES}
    ${SCINTILLA_LEXLIB_SOURCES}
    ${SCINTILLA_LEXERS_SOURCES}
)

target_compile_definitions(qscintilla2_qt6 PRIVATE
    QSCINTILLA_MAKE_DLL
    SCINTILLA_QT
    SCI_LEXER
    INCLUDE_DEPRECATED_FEATURES
)

target_include_directories(qscintilla2_qt6 PUBLIC
    "${QSCI_DIR}"
    "${SCINTILLA_DIR}/include"
    "${SCINTILLA_DIR}/lexlib"
    "${SCINTILLA_DIR}/src"
)

target_link_libraries(qscintilla2_qt6 PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::PrintSupport
)

if(MSVC)
    target_compile_options(qscintilla2_qt6 PRIVATE /utf-8 /wd4091 /wd4244 /wd4267 /wd4996)
endif()

install(TARGETS qscintilla2_qt6
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
install(DIRECTORY "${QSCI_DIR}/Qsci" DESTINATION include)
