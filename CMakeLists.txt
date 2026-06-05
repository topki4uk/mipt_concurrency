cmake_minimum_required(VERSION 3.10)
project(MiptConcurrancy VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(simple_server src/simple_server.cpp)
add_executable(thread_server src/thread_server.cpp)
add_executable(simple_client src/simple_client.cpp)