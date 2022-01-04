cmake_minimum_required(VERSION 3.16)

project(simple-http-server LANGUAGES CXX VERSION 1.0.0)

add_executable(simple-http-server main.cpp)

target_compile_options(simple-http-server PUBLIC -O3 -Wall)

target_compile_features(simple-http-server PUBLIC cxx_std_17)

find_package(Threads REQUIRED)

target_link_libraries(simple-http-server PRIVATE Threads::Threads)
