cmake_minimum_required(VERSION 3.16)
project(roscoppe VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

add_executable(roscoppe
    src/main.cpp
    src/ui/main_window.cpp
    src/services/command_runner.cpp
    src/services/process_manager.cpp
    src/services/ros_inspector.cpp
    src/services/diagnostics_engine.cpp
    src/services/snapshot_diff.cpp
    src/services/session_recorder.cpp
    src/services/remote_monitor.cpp
    src/services/runtime_worker.cpp
    src/services/system_monitor.cpp
    src/services/health_monitor.cpp
    src/services/control_actions.cpp
    src/services/snapshot_manager.cpp
)

target_include_directories(roscoppe PRIVATE include)

target_link_libraries(roscoppe PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
)

target_compile_options(roscoppe PRIVATE -Wall -Wextra -Wpedantic)
