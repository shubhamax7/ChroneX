cmake_minimum_required(VERSION 3.24)

project(ChroneX VERSION 0.0.0 LANGUAGES CXX)

option(CHRONEX_BUILD_TESTS "Build ChroneX tests" ON)
option(CHRONEX_BUILD_BENCHMARKS "Build ChroneX benchmarks" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(chronex_options INTERFACE)
target_compile_features(chronex_options INTERFACE cxx_std_20)

if(MSVC)
  target_compile_options(chronex_options INTERFACE /W4 /permissive- /EHsc)
else()
  target_compile_options(chronex_options INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wold-style-cast
    -Wcast-align
    -Woverloaded-virtual
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
  )
endif()

# This target deliberately has no implementation in P0. It makes the language
# standard and warning policy consumable as the domain model arrives in P1.
add_library(chronex INTERFACE)
target_link_libraries(chronex INTERFACE chronex_options)
target_include_directories(chronex INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

include(CTest)

if(BUILD_TESTING OR CHRONEX_BUILD_BENCHMARKS)
  include(FetchContent)

  # Pin named release versions. Updating either dependency is an explicit
  # reviewable change, never an accidental floating-main update.
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
  )
  FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.4
    GIT_SHALLOW TRUE
  )

  if(BUILD_TESTING)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  if(CHRONEX_BUILD_BENCHMARKS)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googlebenchmark)
  endif()
endif()
