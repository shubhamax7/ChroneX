cmake_minimum_required(VERSION 3.24)
project(ChroneXConsumer LANGUAGES CXX)

find_package(ChroneX CONFIG REQUIRED)

add_executable(chronex_package_consumer main.cpp)
target_compile_features(chronex_package_consumer PRIVATE cxx_std_20)
target_link_libraries(chronex_package_consumer PRIVATE ChroneX::chronex)

enable_testing()
add_test(NAME installed_package_consumer COMMAND chronex_package_consumer)
