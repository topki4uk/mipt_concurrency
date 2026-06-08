cmake_minimum_required(VERSION 3.10)
project(MiptConcurrency VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(simple_server src/simple_server.cpp)
add_executable(thread_server src/thread_server.cpp)
add_executable(ub_example src/ub_example.cpp)
add_executable(simple_client src/simple_client.cpp)
add_executable(epoll_server src/epoll_server.cpp)