cmake_minimum_required(VERSION 3.16)
project(nba_lab VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native")

find_package(Threads REQUIRED)

# nlohmann/json (header only)
include_directories(${CMAKE_SOURCE_DIR}/include)

file(GLOB_RECURSE SOURCES "src/*.cpp")
add_executable(nba_lab ${SOURCES})
target_link_libraries(nba_lab Threads::Threads)
