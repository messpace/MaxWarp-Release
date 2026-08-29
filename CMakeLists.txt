# MaxWarp — A330 sim-rate fuel-burn corrector for MSFS 2024.
#
# This is the PUBLIC build: the application target and nothing else. The private
# development tree's CMakeLists.txt also defines four headless test suites and
# two bench tools, none of which are published, so shipping it verbatim would
# fail to configure on a fresh clone.
#
# The qt_add_executable source list below is asserted against the private one by
# the release wizard (stage 4). A source added to the app there and not here
# aborts the release rather than publishing a tree that does not build.
cmake_minimum_required(VERSION 3.21)
project(MaxWarp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Both are overridable cache paths. SimConnect defaults to the copy vendored in
# third_party/ so a machine with no MSFS 2024 SDK — every CI runner — still
# builds; point it at your own SDK if you have one.
set(MAXWARP_QT_ROOT "C:/Qt/6.10.2/msvc2022_64" CACHE PATH "Qt 6 (MSVC x64) root directory")
set(SIMCONNECT_SDK_ROOT "${CMAKE_SOURCE_DIR}/third_party/SimConnect" CACHE PATH "SimConnect SDK root directory")
list(APPEND CMAKE_PREFIX_PATH "${MAXWARP_QT_ROOT}")

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Network Qml Quick)
qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(MaxWarp
    src/main.cpp
    src/Corrector.h
    src/Corrector.cpp
    src/LVarBridge.h
    src/LVarBridge.cpp
    src/RateDriver.h
    src/RateDriver.cpp
    src/SessionLogger.h
    src/SessionLogger.cpp
    src/SimBriefClient.h
    src/SimBriefClient.cpp
    src/SimBriefPlan.h
    src/SimBriefPlan.cpp
    src/SimConnectWorker.h
    src/SimConnectWorker.cpp
    src/SimService.h
    src/SimService.cpp
    src/ThemeController.h
    src/ThemeController.cpp
    src/TurnGovernor.h
    src/TurnGovernor.cpp
)

qt_add_qml_module(MaxWarp
    URI MaxWarp
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

# Identity fonts: Martian Mono + Instrument Sans, both OFL (licenses in fonts/).
# Loaded at startup from :/fonts.
qt_add_resources(MaxWarp "fonts"
    PREFIX "/fonts"
    BASE fonts
    FILES
        fonts/InstrumentSans-Regular.ttf
        fonts/InstrumentSans-Medium.ttf
        fonts/InstrumentSans-SemiBold.ttf
        fonts/InstrumentSans-Bold.ttf
        fonts/MartianMono-StdLt.ttf
        fonts/MartianMono-StdRg.ttf
        fonts/MartianMono-StdMd.ttf
        fonts/MartianMono-StdBd.ttf
)

target_include_directories(MaxWarp PRIVATE "${SIMCONNECT_SDK_ROOT}/include")
target_link_libraries(MaxWarp PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    "${SIMCONNECT_SDK_ROOT}/lib/SimConnect.lib"
)

add_custom_command(TARGET MaxWarp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SIMCONNECT_SDK_ROOT}/lib/SimConnect.dll"
        "$<TARGET_FILE_DIR:MaxWarp>")
