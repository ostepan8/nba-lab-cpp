cmake_minimum_required(VERSION 3.16)
project(nba_lab VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native")

find_package(Threads REQUIRED)

# nlohmann/json (header only)
include_directories(${CMAKE_SOURCE_DIR}/include)

# Optional: LightGBM C API
find_library(LGBM_LIB lightgbm)
if(LGBM_LIB)
    message(STATUS "LightGBM found: ${LGBM_LIB}")
else()
    message(STATUS "LightGBM not found -- using heuristic fallbacks for ML strategies")
endif()

# SQLite3 amalgamation (included as source -- no system package needed)
set(SQLITE3_SRC "${CMAKE_SOURCE_DIR}/include/sqlite3.c")

file(GLOB_RECURSE SOURCES "src/*.cpp")
add_executable(nba_lab ${SOURCES} ${SQLITE3_SRC})
target_link_libraries(nba_lab Threads::Threads dl)

if(LGBM_LIB)
    target_link_libraries(nba_lab ${LGBM_LIB})
    target_compile_definitions(nba_lab PRIVATE HAS_LIGHTGBM=1)
endif()
