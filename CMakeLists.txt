cmake_minimum_required(VERSION 3.16)
project(fronthaul_sniffer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(FRONTHAUL_ENABLE_PCAP "Enable libpcap backend" ON)
option(FRONTHAUL_ENABLE_SOCKET "Enable raw socket backend" ON)

add_executable(fronthaul_sniffer
    src/main.cpp
    src/capture/c_capture.c
    src/pipeline/c_packet_queue.c
    src/pipeline/packet_processor.cpp
)

target_include_directories(fronthaul_sniffer PRIVATE include)

target_compile_options(fronthaul_sniffer PRIVATE -Wall -Wextra -Wpedantic)

target_compile_definitions(fronthaul_sniffer PRIVATE
    _DEFAULT_SOURCE
    _POSIX_C_SOURCE=200809L
    $<$<BOOL:${FRONTHAUL_ENABLE_PCAP}>:FRONTHAUL_ENABLE_PCAP=1>
    $<$<BOOL:${FRONTHAUL_ENABLE_SOCKET}>:FRONTHAUL_ENABLE_SOCKET=1>
)

if(FRONTHAUL_ENABLE_PCAP)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PCAP QUIET libpcap)
    endif()

    if(PCAP_FOUND)
        target_include_directories(fronthaul_sniffer PRIVATE ${PCAP_INCLUDE_DIRS})
        target_link_libraries(fronthaul_sniffer PRIVATE ${PCAP_LIBRARIES})
        target_compile_options(fronthaul_sniffer PRIVATE ${PCAP_CFLAGS_OTHER})
    else()
        find_library(PCAP_LIBRARY NAMES pcap)
        if(NOT PCAP_LIBRARY)
            message(FATAL_ERROR "libpcap not found. Install libpcap-dev or disable FRONTHAUL_ENABLE_PCAP.")
        endif()
        target_link_libraries(fronthaul_sniffer PRIVATE ${PCAP_LIBRARY})
    endif()
endif()

target_link_libraries(fronthaul_sniffer PRIVATE pthread)
