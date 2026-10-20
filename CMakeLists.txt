cmake_minimum_required(VERSION 3.16)
project(fronthaul_sniffer VERSION 0.1.0 LANGUAGES C CXX)

# Without this the project builds at -O0, which costs far more than any tuning
# flag below can win back. RelWithDebInfo rather than Release: -O3 buys little
# over -O2 here, and keeping -g means a crash still yields a usable backtrace.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Tunes the radar DSP core for the building machine's CPU. Off by default: it
# makes the binary SIGILL on any older CPU, and this tool is meant to be ported.
option(FH_NATIVE_ARCH "Tune the radar DSP core for this machine's CPU (non-portable binary)" OFF)

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
    src/capture/c_channel_filter.c
    src/pipeline/c_packet_queue.c
    src/core/c_fronthaul_decoder.c
    src/core/bfp_decompress.c
    src/core/c_oran_memory.c
    src/core/c_radar.c
    src/core/c_radar_dsp.c
    src/analysis/c_iq_analysis.c
    src/visualization/opengl_visualizer.cpp
    src/visualization/iq_constellation.cpp
    src/gui/imgui_startup_interface.cpp
    src/gui/imgui_visualizer_interface.cpp
    src/gui/imgui_constellation_interface.cpp
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

# Lets sqrtf compile to a single instruction instead of a call plus an errno
# branch. Every magnitude in this codebase is a sqrtf, and nothing reads errno
# after one. Deliberately NOT -ffast-math: that would assume no NaN/Inf ever
# occurs, which would fold away zoom_guard()'s NaN rejection and break the
# radar's detection of 0/0 on unoccupied resource elements.
target_compile_options(fronthaul_sniffer PRIVATE -fno-math-errno)

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

# Single-precision FFTW for the radar DSP core. fh_complex_t is already
# 'float complex', which is layout-compatible with fftwf_complex, so the symbol
# ring feeds the transforms with no conversion pass.
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)
target_link_libraries(fronthaul_sniffer PRIVATE PkgConfig::FFTW3F)

# The threading half of FFTW ships without a pkg-config file on Debian/Ubuntu,
# so it has to be located by hand rather than via pkg_check_modules.
find_library(FFTW3F_THREADS_LIBRARY NAMES fftw3f_threads REQUIRED)
target_link_libraries(fronthaul_sniffer PRIVATE ${FFTW3F_THREADS_LIBRARY})

if(FH_NATIVE_ARCH)
    # Scoped to the DSP core alone: FFTW already picks its SIMD codelets at
    # runtime, so -march=native only reaches the loops written here.
    set_source_files_properties(src/core/c_radar.c src/core/c_radar_dsp.c PROPERTIES COMPILE_OPTIONS "-march=native")
endif()
