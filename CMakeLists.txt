cmake_minimum_required(VERSION 3.16)
project(fronthaul_sniffer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(fronthaul_sniffer
    src/main.cpp
    src/capture/c_capture_socket_mode.c
    src/pipeline/c_packet_queue.c
    src/core/c_fronthaul_decoder.c
    src/core/bfp_decompress.c
    src/core/c_oran_memory.c
    src/visualization/opengl_visualizer.cpp
)

target_include_directories(fronthaul_sniffer PRIVATE src)

target_compile_options(fronthaul_sniffer PRIVATE -Wall -Wextra -Wpedantic)

target_compile_definitions(fronthaul_sniffer PRIVATE
    _DEFAULT_SOURCE
    _POSIX_C_SOURCE=200809L
)

target_link_libraries(fronthaul_sniffer PRIVATE pthread)
target_link_libraries(fronthaul_sniffer PRIVATE m)

# Find GLFW and OpenGL for visualization
find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)
target_link_libraries(fronthaul_sniffer PRIVATE glfw OpenGL::GL)
