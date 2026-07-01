cmake_minimum_required(VERSION 3.16)
project(queue_drain_tool LANGUAGES CXX)

find_package(Threads REQUIRED)

add_executable(queue_drain queue_drain.cpp)
target_compile_features(queue_drain PRIVATE cxx_std_17)

target_link_libraries(queue_drain PRIVATE Threads::Threads)
