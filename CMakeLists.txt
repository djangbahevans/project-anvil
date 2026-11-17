cmake_minimum_required(VERSION 3.25)

# Use the vcpkg toolchain when a vcpkg checkout is available; otherwise
# dependencies resolve from system packages.
if(NOT CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT}
   AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "")
endif()

project(anvil VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

set(ANVIL_BUILD_UI AUTO CACHE STRING "Build the Qt UI and application: AUTO, ON or OFF")
set_property(CACHE ANVIL_BUILD_UI PROPERTY STRINGS AUTO ON OFF)
option(ANVIL_BUILD_TESTS "Build unit tests" ON)
option(ANVIL_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(ANVIL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(ANVIL_SANITIZE "Enable address and undefined-behavior sanitizers" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(AnvilCompileOptions)

add_subdirectory(src)

if(ANVIL_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()

if(ANVIL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
