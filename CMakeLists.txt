cmake_minimum_required(VERSION 3.18)
project(UniNet LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Build everything position-independent so the static core (and a fetched cnats)
# can link into the shared C ABI lib and the Python extension.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ── Device/feature-adaptive: like UniVox, optional tiers are auto-detected.
# zlib is the always-available baseline (deflate codec); liblz4 is the faster tier
# (auto-detected via pkg-config); cnats is the production transport (off by default,
# fetched when enabled). The library always builds CPU-only with the loopback bus.
find_package(ZLIB REQUIRED)

option(UNINET_LZ4  "Use liblz4 (faster compression tier)" ON)
if(UNINET_LZ4)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LZ4 IMPORTED_TARGET liblz4)
    endif()
    if(LZ4_FOUND)
        message(STATUS "UniNet: liblz4 found — LZ4 compression tier enabled.")
    else()
        message(STATUS "UniNet: liblz4 NOT found — zlib + none only.")
    endif()
endif()

option(UNINET_NATS "Build the NATS transport (fetches cnats)" OFF)
if(UNINET_NATS)
    include(FetchContent)
    FetchContent_Declare(cnats GIT_REPOSITORY https://github.com/nats-io/nats.c.git GIT_TAG v3.9.1)
    set(NATS_BUILD_STREAMING OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(cnats)
    set(UNINET_HAS_NATS ON)
    message(STATUS "UniNet: NATS transport enabled (cnats fetched).")
endif()

add_library(uninet STATIC
    src/cbor.cpp
    src/codec.cpp
    src/loopback.cpp
    src/node.cpp
    src/profiler.cpp
    src/nats_transport.cpp)        # empty TU unless UNINET_HAS_NATS
set_target_properties(uninet PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(uninet PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(uninet PUBLIC ZLIB::ZLIB)
if(LZ4_FOUND AND UNINET_LZ4)
    target_link_libraries(uninet PUBLIC PkgConfig::LZ4)
    target_compile_definitions(uninet PUBLIC UNINET_HAS_LZ4=1)
endif()
if(UNINET_NATS)
    target_compile_definitions(uninet PUBLIC UNINET_HAS_NATS=1)
    target_link_libraries(uninet PUBLIC nats_static)
endif()
target_compile_options(uninet PRIVATE -Wall -Wextra)

# ── Tests ──
enable_testing()
add_executable(test_roundtrip tests/test_roundtrip.cpp)
target_link_libraries(test_roundtrip PRIVATE uninet)
add_test(NAME roundtrip COMMAND test_roundtrip)

# ── Benchmark (README performance matrix) ──
add_executable(benchmark tests/benchmark.cpp)
target_link_libraries(benchmark PRIVATE uninet)

# ── C ABI shared library (for C# / P/Invoke and other FFI). ──
option(UNINET_BUILD_CABI "Build the C ABI shared library (libuninet_c)" OFF)
if(UNINET_BUILD_CABI)
    add_library(uninet_c SHARED src/cabi.cpp)
    target_link_libraries(uninet_c PUBLIC uninet)
    set_target_properties(uninet_c PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        OUTPUT_NAME uninet_c)
    if(UNINET_NATS)
        target_compile_definitions(uninet_c PRIVATE UNINET_HAS_NATS=1)
    endif()
endif()

# ── Python extension (pybind11). Built when UNINET_BUILD_PYTHON=ON; `pip install`
# drives it via scikit-build-core. ──
option(UNINET_BUILD_PYTHON "Build the pybind11 Python extension (_uninet)" OFF)
if(UNINET_BUILD_PYTHON)
    find_package(pybind11 CONFIG REQUIRED)
    pybind11_add_module(_uninet python/bindings.cpp)
    target_link_libraries(_uninet PRIVATE uninet)
    set_target_properties(_uninet PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/python/uninet")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
