cmake_minimum_required(VERSION 3.16)
project(qfcoro LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# 指向本机自定义 Boost
list(APPEND CMAKE_PREFIX_PATH
  "/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1")

find_package(Boost 1.74 CONFIG REQUIRED COMPONENTS fiber context)
find_package(Qt5 REQUIRED COMPONENTS Core Test Concurrent)

add_library(qfcoro src/core.cpp)
target_include_directories(qfcoro PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(qfcoro PUBLIC Qt5::Core Boost::fiber Boost::context)

enable_testing()
add_subdirectory(tests)
add_subdirectory(examples)
