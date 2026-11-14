cmake_minimum_required(VERSION 3.24)

project(ChroneX VERSION 0.8.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

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

# The Phase 2 reference implementation intentionally favors inspectable,
# standards-based containers over optimized storage policies.
add_library(chronex STATIC src/oracle/reference_book.cpp)
add_library(ChroneX::chronex ALIAS chronex)
target_sources(chronex PRIVATE
  src/book/optimized_book.cpp
  src/journal/command_journal.cpp
  src/market_data/depth_reconstructor.cpp
)
target_link_libraries(chronex PUBLIC Threads::Threads)
target_compile_options(chronex PRIVATE
  $<TARGET_PROPERTY:chronex_options,INTERFACE_COMPILE_OPTIONS>
)
target_include_directories(chronex PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS chronex
  EXPORT ChroneXTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/ChroneX)
install(EXPORT ChroneXTargets
  FILE ChroneXTargets.cmake
  NAMESPACE ChroneX::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ChroneX
)

configure_package_config_file(
  cmake/ChroneXConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/ChroneXConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ChroneX
)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/ChroneXConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMinorVersion
)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/ChroneXConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/ChroneXConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ChroneX
)

include(CTest)

if(BUILD_TESTING OR CHRONEX_BUILD_BENCHMARKS)
  include(FetchContent)

  if(BUILD_TESTING)
    FetchContent_Declare(
      googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.17.0
      GIT_SHALLOW TRUE
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(chronex_reference_book_test
      tests/golden/reference_book_test.cpp
      tests/boundary/reference_book_boundary_test.cpp
      tests/property/optimized_book_differential_test.cpp
      tests/pipeline/phase4_test.cpp
    )
    target_link_libraries(chronex_reference_book_test PRIVATE chronex chronex_options GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(chronex_reference_book_test)

  endif()

  if(CHRONEX_BUILD_BENCHMARKS)
    FetchContent_Declare(
      googlebenchmark
      GIT_REPOSITORY https://github.com/google/benchmark.git
      GIT_TAG v1.9.4
      GIT_SHALLOW TRUE
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googlebenchmark)
    target_compile_definitions(chronex PRIVATE CHRONEX_ENABLE_ALLOCATION_COUNTER)
    add_executable(chronex_benchmarks benchmarks/reference_book_benchmark.cpp)
    target_link_libraries(chronex_benchmarks PRIVATE chronex chronex_options benchmark::benchmark_main)
  endif()
endif()
