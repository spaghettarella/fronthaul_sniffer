cmake_minimum_required(VERSION 3.16)
project(fronthaul_sniffer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Fetch ImGui for stream selection interface
include(FetchContent)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/v1.89.8.tar.gz
)
FetchContent_MakeAvailable(imgui)

add_executable(fronthaul_sniffer
    src/main.cpp
    src/capture/c_capture_socket_mode.c
    src/capture/c_capture_ocudu_phy_tap.c
    src/capture/c_pcap_demo_stream.c
    src/capture/c_capture_loopback_mode.c
    src/pipeline/c_packet_queue.c
    src/core/c_fronthaul_decoder.c
    src/core/bfp_decompress.c
    src/core/c_oran_memory.c
    src/visualization/opengl_visualizer.cpp
    src/gui/imgui_startup_interface.cpp
    src/gui/imgui_visualizer_interface.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl2.cpp
)

target_include_directories(fronthaul_sniffer PRIVATE src)
target_include_directories(fronthaul_sniffer PRIVATE ${imgui_SOURCE_DIR})
target_include_directories(fronthaul_sniffer PRIVATE ${imgui_SOURCE_DIR}/backends)

target_compile_options(fronthaul_sniffer PRIVATE -Wall -Wextra -Wpedantic)

target_compile_definitions(fronthaul_sniffer PRIVATE
    _DEFAULT_SOURCE
    _POSIX_C_SOURCE=200809L
    IMGUI_IMPL_OPENGL_LOADER_GLAD
)

target_link_libraries(fronthaul_sniffer PRIVATE pthread)
target_link_libraries(fronthaul_sniffer PRIVATE m)

find_library(PCAP_LIBRARY NAMES pcap REQUIRED)
target_link_libraries(fronthaul_sniffer PRIVATE ${PCAP_LIBRARY})

# Find GLFW and OpenGL for visualization
find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)
target_link_libraries(fronthaul_sniffer PRIVATE glfw OpenGL::GL)
