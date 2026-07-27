cmake_minimum_required(VERSION 3.18)
project(UniNet LANGUAGES C CXX VERSION 0.2.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Build everything position-independent so the static core (and a fetched zyre)
# can link into the shared C ABI lib and the Python extension.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Set before any target is created: CMake latches this per target at creation.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ── Warnings, portably. The old build hardcoded GCC flags on the core target
# only, which is why the tests and bindings compiled with none at all.
add_library(uninet_warnings INTERFACE)
if(MSVC)
    target_compile_options(uninet_warnings INTERFACE /W4)
else()
    target_compile_options(uninet_warnings INTERFACE -Wall -Wextra)
endif()

# ── Compression tiers. zlib is the always-available baseline; liblz4 is the
# faster tier and is auto-detected. Neither is required to use the network.
find_package(ZLIB REQUIRED)

option(UNINET_LZ4 "Use liblz4 (faster compression tier)" ON)
if(UNINET_LZ4)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LZ4 IMPORTED_TARGET liblz4)
    endif()
    if(NOT LZ4_FOUND)
        # vcpkg (and some distros) ship a CMake config instead of a .pc file, so
        # a missing pkg-config is not the same as a missing liblz4.
        find_package(lz4 CONFIG QUIET)
        if(lz4_FOUND)
            set(LZ4_FOUND TRUE)
            set(UNINET_LZ4_TARGET lz4::lz4)
        endif()
    else()
        set(UNINET_LZ4_TARGET PkgConfig::LZ4)
    endif()
    if(LZ4_FOUND)
        message(STATUS "UniNet: liblz4 found — LZ4 compression tier enabled.")
    else()
        message(STATUS "UniNet: liblz4 NOT found — falling back to zlib.")
    endif()
endif()

# ── The network: ZeroMQ/Zyre (ZRE). This is what makes UniNet brokerless —
# nodes discover each other with a UDP beacon and connect directly. Preferred
# from the system; built from source when absent, so a fresh machine needs no
# manual dependency install.
option(UNINET_SYSTEM_ZYRE "Use a system-installed zyre if present" ON)
set(UNINET_ZYRE_TARGET "")
if(UNINET_SYSTEM_ZYRE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ZYRE IMPORTED_TARGET libzyre)
    endif()
    if(ZYRE_FOUND)
        # libzyre.pc lists czmq/libzmq only as private requirements, which
        # pkg-config does not expand for a dynamic link — so linking against
        # zyre alone fails with "DSO missing from command line". Ask for them.
        pkg_check_modules(CZMQ IMPORTED_TARGET libczmq)
        pkg_check_modules(ZMQ  IMPORTED_TARGET libzmq)
        set(UNINET_ZYRE_TARGET PkgConfig::ZYRE)
        if(CZMQ_FOUND)
            list(APPEND UNINET_ZYRE_TARGET PkgConfig::CZMQ)
        endif()
        if(ZMQ_FOUND)
            list(APPEND UNINET_ZYRE_TARGET PkgConfig::ZMQ)
        endif()
        message(STATUS "UniNet: using system zyre ${ZYRE_VERSION}.")
    endif()
endif()
if(NOT UNINET_ZYRE_TARGET)
    message(STATUS "UniNet: zyre not found — building libzmq + czmq + zyre from source.")
    include(FetchContent)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED  OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC  ON  CACHE BOOL "" FORCE)
    set(WITH_DOC      OFF CACHE BOOL "" FORCE)
    set(ENABLE_DRAFTS OFF CACHE BOOL "" FORCE)
    set(ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WITH_PERF_TOOL  OFF CACHE BOOL "" FORCE)
    set(CZMQ_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(CZMQ_BUILD_STATIC ON  CACHE BOOL "" FORCE)
    set(ZYRE_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZYRE_BUILD_STATIC ON  CACHE BOOL "" FORCE)
    FetchContent_Declare(libzmq
        GIT_REPOSITORY https://github.com/zeromq/libzmq.git GIT_TAG v4.3.5)
    FetchContent_Declare(czmq
        GIT_REPOSITORY https://github.com/zeromq/czmq.git   GIT_TAG v4.2.1)
    FetchContent_Declare(zyre
        GIT_REPOSITORY https://github.com/zeromq/zyre.git   GIT_TAG v2.0.1)
    FetchContent_MakeAvailable(libzmq czmq zyre)
    set(UNINET_ZYRE_TARGET zyre-static)
endif()

add_library(uninet STATIC
    src/blob.cpp
    src/cbor.cpp
    src/codec.cpp
    src/json.cpp
    src/loopback.cpp
    src/node.cpp
    src/profiler.cpp
    src/session.cpp
    src/zyre_transport.cpp)
set_target_properties(uninet PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(uninet PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(uninet PUBLIC ZLIB::ZLIB ${UNINET_ZYRE_TARGET})
target_link_libraries(uninet PRIVATE uninet_warnings)
if(LZ4_FOUND AND UNINET_LZ4)
    target_link_libraries(uninet PUBLIC ${UNINET_LZ4_TARGET})
    target_compile_definitions(uninet PUBLIC UNINET_HAS_LZ4=1)
endif()
# zlib is a hard requirement above, so the Zlib tier is always present. Without
# this the "fall back to zlib" default silently selected None instead.
target_compile_definitions(uninet PUBLIC UNINET_HAS_ZLIB=1)
if(WIN32)
    target_link_libraries(uninet PUBLIC ws2_32 iphlpapi rpcrt4)

    # Linking the ZeroMQ stack statically on Windows requires these, and the
    # failure without them is obscure: every symbol is declared
    # __declspec(dllimport), so the link dies with "undefined reference to
    # __imp_zyre_new" and friends. vcpkg's default x64-windows triplet is DLL,
    # so this stays off unless you asked for static.
    option(UNINET_ZMQ_STATIC "Link ZeroMQ/czmq/zyre statically (Windows)" OFF)
    if(UNINET_ZMQ_STATIC)
        target_compile_definitions(uninet PUBLIC ZMQ_STATIC CZMQ_STATIC ZYRE_STATIC)
    endif()
endif()

# ── Tests ──
enable_testing()
add_executable(test_roundtrip tests/test_roundtrip.cpp)
target_link_libraries(test_roundtrip PRIVATE uninet uninet_warnings)
add_test(NAME roundtrip COMMAND test_roundtrip)

add_executable(test_network tests/test_network.cpp)
target_link_libraries(test_network PRIVATE uninet uninet_warnings)
add_test(NAME network COMMAND test_network)
# Two nodes must find each other over a real UDP beacon; give it room on a
# loaded CI box before calling it a failure.
set_tests_properties(network PROPERTIES TIMEOUT 90)

# ── Benchmarks ──
# `benchmark` measures the codec in isolation; `uninet-benchmark` measures what a
# user experiences: two real nodes exchanging real mesh payloads over the network.
add_executable(benchmark tests/benchmark.cpp)
target_link_libraries(benchmark PRIVATE uninet uninet_warnings)

add_executable(benchmark_network tests/benchmark_network.cpp)
target_link_libraries(benchmark_network PRIVATE uninet uninet_warnings)
set_target_properties(benchmark_network PROPERTIES OUTPUT_NAME uninet-benchmark)

# ── The CLI and the demo ──
# uninet-discover answers "what is on my network?"; uninet-demo is two or more
# devices talking with nothing configured. See scripts/demo.sh.
add_executable(uninet_discover tools/uninet_discover.cpp)
target_link_libraries(uninet_discover PRIVATE uninet uninet_warnings)
set_target_properties(uninet_discover PROPERTIES OUTPUT_NAME uninet-discover)

add_executable(uninet_demo examples/demo.cpp)
target_link_libraries(uninet_demo PRIVATE uninet uninet_warnings)
set_target_properties(uninet_demo PROPERTIES OUTPUT_NAME uninet-demo)

add_executable(uninet_file_transfer examples/file_transfer.cpp)
target_link_libraries(uninet_file_transfer PRIVATE uninet uninet_warnings)
set_target_properties(uninet_file_transfer PROPERTIES OUTPUT_NAME uninet-file-transfer)

# ── C ABI shared library (for C# / P-Invoke and other FFI). ──
option(UNINET_BUILD_CABI "Build the C ABI shared library (libuninet_c)" OFF)
if(UNINET_BUILD_CABI)
    add_library(uninet_c SHARED src/cabi.cpp)
    target_link_libraries(uninet_c PUBLIC uninet)
    target_link_libraries(uninet_c PRIVATE uninet_warnings)
    set_target_properties(uninet_c PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        OUTPUT_NAME uninet_c)

    # Compiled as C, so it proves the header really is C-consumable and the
    # extern "C" surface links the way P/Invoke will call it.
    add_executable(test_cabi tests/test_cabi.c)
    target_link_libraries(test_cabi PRIVATE uninet_c uninet_warnings)
    set_target_properties(test_cabi PROPERTIES C_STANDARD 99)
    add_test(NAME cabi COMMAND test_cabi)
    set_tests_properties(cabi PROPERTIES TIMEOUT 90)
endif()

# ── Python extension (pybind11). `pip install` drives it via scikit-build-core.
option(UNINET_BUILD_PYTHON "Build the pybind11 Python extension (_uninet)" OFF)
if(UNINET_BUILD_PYTHON)
    find_package(pybind11 CONFIG REQUIRED)
    pybind11_add_module(_uninet python/bindings.cpp)
    target_link_libraries(_uninet PRIVATE uninet)
    set_target_properties(_uninet PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/python/uninet")
endif()

# ── Install rules, so consumers can find_package(UniNet) instead of vendoring
# a file copy (which is how ThermoNavServer ended up with a diverging fork).
include(GNUInstallDirs)
install(TARGETS uninet EXPORT UniNetTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/uninet DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS uninet_discover RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNINET_BUILD_CABI)
    install(TARGETS uninet_c LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                             RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ── Cross-language interop ──
# The C++ participant of scripts/test-interop.sh, which runs a C++, a Python and
# a C# node in one realm and checks that each decodes the others' payloads
# byte-identically. Not an add_test(): it needs the other language runtimes, so
# the shell script drives it and reports which languages were actually covered.
add_executable(interop_cpp tests/interop/interop_cpp.cpp)
target_link_libraries(interop_cpp PRIVATE uninet uninet_warnings)
