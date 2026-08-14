cmake_minimum_required(VERSION 3.20)
project(transport-runpod CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)

add_executable(rp-worker src/worker.cpp)
target_link_libraries(rp-worker PRIVATE CURL::libcurl)
