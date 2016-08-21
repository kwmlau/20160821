cmake_minimum_required(VERSION 3.5)
project(20160820)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -pthread")

set(SOURCE_FILES main.cpp)
add_executable(20160820 ${SOURCE_FILES})