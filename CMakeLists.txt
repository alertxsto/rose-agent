cmake_minimum_required(VERSION 3.24)
project(RoseAgent VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(ROSE_QT_MAJOR "6" CACHE STRING "Qt major version: 6 (modern) or 5 (Windows 8.1 legacy)")
set_property(CACHE ROSE_QT_MAJOR PROPERTY STRINGS 5 6)
if(ROSE_QT_MAJOR STREQUAL "5")
    set(ROSE_QT_PACKAGE Qt5)
    set(ROSE_QT_MIN_VERSION 5.15)
elseif(ROSE_QT_MAJOR STREQUAL "6")
    set(ROSE_QT_PACKAGE Qt6)
    set(ROSE_QT_MIN_VERSION 6.5)
else()
    message(FATAL_ERROR "ROSE_QT_MAJOR must be 5 or 6")
endif()
find_package(${ROSE_QT_PACKAGE} ${ROSE_QT_MIN_VERSION} REQUIRED COMPONENTS Core)
if(ROSE_QT_MAJOR STREQUAL "6")
    # Legacy Rose encodings must work on Windows too, without depending on
    # whether a particular Qt Core build happened to include ICU codecs.
    find_package(Qt6 ${ROSE_QT_MIN_VERSION} REQUIRED COMPONENTS Core5Compat)
endif()
if(MSVC)
    # Source strings are UTF-8 on every host, including MSVC 2019/v142.
    add_compile_options(/utf-8 /Zc:__cplusplus)
endif()
if(WIN32 AND ROSE_QT_MAJOR STREQUAL "5")
    add_compile_definitions(_WIN32_WINNT=0x0603 WINVER=0x0603 NTDDI_VERSION=0x06030000)
endif()
include(CTest)

add_library(rose_workspace
    core/PetalDocument.cpp
    core/Model.cpp
    core/ControlledUnits.cpp
    core/Workspace.cpp
    core/Json.cpp
    core/Storage.cpp)
target_include_directories(rose_workspace PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rose_workspace PUBLIC ${ROSE_QT_PACKAGE}::Core)
if(ROSE_QT_MAJOR STREQUAL "6")
    target_link_libraries(rose_workspace PUBLIC Qt6::Core5Compat)
endif()
target_compile_options(rose_workspace PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
if(WIN32)
    target_link_libraries(rose_workspace PRIVATE Advapi32)
endif()

add_library(rose_controller
    desktop/WorkspaceController.cpp
    desktop/WorkspaceWorker.cpp)
target_link_libraries(rose_controller PUBLIC rose_workspace ${ROSE_QT_PACKAGE}::Core)

find_package(${ROSE_QT_PACKAGE} ${ROSE_QT_MIN_VERSION} REQUIRED COMPONENTS Concurrent Network Gui Widgets PrintSupport Svg)
add_library(rose_agent
    agent/AgentTypes.cpp
    agent/Provider.cpp
    agent/ProviderSettings.cpp
    agent/Credentials.cpp
    agent/AgentHistory.cpp
    agent/AgentTools.cpp
    agent/AgentRun.cpp
    agent/AgentSkill.cpp
    agent/skills.qrc)
target_link_libraries(rose_agent PUBLIC rose_controller ${ROSE_QT_PACKAGE}::Concurrent ${ROSE_QT_PACKAGE}::Network ${ROSE_QT_PACKAGE}::Gui)
if(WIN32)
    target_link_libraries(rose_agent PRIVATE Advapi32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(${ROSE_QT_PACKAGE} REQUIRED COMPONENTS DBus)
    target_link_libraries(rose_agent PRIVATE ${ROSE_QT_PACKAGE}::DBus)
endif()

add_library(rose_widgets
    desktop/MainWindow.cpp
    desktop/DiagramScene.cpp
    desktop/Printing.cpp
    agent/AgentDock.cpp
    agent/ProviderDialog.cpp)
target_link_libraries(rose_widgets PUBLIC rose_agent ${ROSE_QT_PACKAGE}::Widgets ${ROSE_QT_PACKAGE}::PrintSupport ${ROSE_QT_PACKAGE}::Svg)
add_executable(rose-agent app/main.cpp)
target_link_libraries(rose-agent PRIVATE rose_widgets)
if(WIN32)
    enable_language(RC)
    set_target_properties(rose-agent PROPERTIES WIN32_EXECUTABLE TRUE)
    target_sources(rose-agent PRIVATE app/windows-resource.rc)
    target_include_directories(rose-agent PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app")
    if(MSVC)
        # The resource contains the versioned compatibility/DPI manifest.
        target_link_options(rose-agent PRIVATE /MANIFEST:NO)
    endif()
endif()

option(ROSE_BUILD_RELEASE_PROBE "Build isolated real GUI/TLS/image release probe" OFF)
if(ROSE_BUILD_RELEASE_PROBE)
    add_executable(rose-release-probe packaging/release-probe.cpp)
    target_link_libraries(rose-release-probe PRIVATE rose_widgets)
endif()

if(BUILD_TESTING)
    find_package(${ROSE_QT_PACKAGE} REQUIRED COMPONENTS Test)
    foreach(suite IN ITEMS petal projection transactions units storage recovery)
        add_executable(tst_${suite} tests/tst_${suite}.cpp)
        target_link_libraries(tst_${suite} PRIVATE rose_workspace ${ROSE_QT_PACKAGE}::Test)
        add_test(NAME ${suite} COMMAND tst_${suite})
    endforeach()
    add_executable(tst_controller desktop/tests/tst_controller.cpp)
    target_link_libraries(tst_controller PRIVATE rose_controller ${ROSE_QT_PACKAGE}::Test)
    add_test(NAME controller COMMAND tst_controller)
    add_executable(tst_canvas desktop/tests/tst_canvas.cpp)
    target_link_libraries(tst_canvas PRIVATE rose_widgets ${ROSE_QT_PACKAGE}::Test)
    add_test(NAME canvas COMMAND tst_canvas)
    set_tests_properties(canvas PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    foreach(suite IN ITEMS providers permissions agent_runs)
        add_executable(tst_${suite} tests/tst_${suite}.cpp)
        target_link_libraries(tst_${suite} PRIVATE rose_agent ${ROSE_QT_PACKAGE}::Test)
        add_test(NAME ${suite} COMMAND tst_${suite})
    endforeach()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
        add_executable(tst_credentials tests/tst_credentials.cpp)
        target_link_libraries(tst_credentials PRIVATE rose_agent ${ROSE_QT_PACKAGE}::Test ${ROSE_QT_PACKAGE}::DBus)
        add_test(NAME credentials COMMAND "${DBUS_RUN_SESSION}" -- "$<TARGET_FILE:tst_credentials>")
        set_tests_properties(credentials PROPERTIES TIMEOUT 120)
    endif()
endif()

add_subdirectory(cli)
if(WIN32 AND ROSE_QT_MAJOR STREQUAL "5")
    if(MSVC)
        target_link_options(rose-agent PRIVATE /SUBSYSTEM:WINDOWS,6.03)
        target_link_options(rose-cli PRIVATE /SUBSYSTEM:CONSOLE,6.03)
        if(TARGET rose-release-probe)
            target_link_options(rose-release-probe PRIVATE /SUBSYSTEM:CONSOLE,6.03)
        endif()
    elseif(MINGW)
        foreach(target IN ITEMS rose-agent rose-cli rose-release-probe)
            if(TARGET ${target})
                target_link_options(${target} PRIVATE -Wl,--major-subsystem-version,6,--minor-subsystem-version,3)
            endif()
        endforeach()
    endif()
endif()

include(cmake/Packaging.cmake)
