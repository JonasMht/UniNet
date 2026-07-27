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

# CI used to pass `-- CXXFLAGS=-Werror` to the build tool. CMake's makefiles read
# CXX_FLAGS (underscore), so that set a variable no recipe ever looked at and
# warnings never once failed the pipeline. This is the real switch.
option(UNINET_WERROR "Treat warnings as errors" OFF)
if(UNINET_WERROR)
    if(MSVC)
        target_compile_options(uninet_warnings INTERFACE /WX)
    else()
        target_compile_options(uninet_warnings INTERFACE -Werror)
    endif()
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
        message(STATUS "UniNet: liblz4 found: LZ4 compression tier enabled.")
    else()
        message(STATUS "UniNet: liblz4 NOT found: falling back to zlib.")
    endif()
endif()

# ── The network: ZeroMQ/Zyre (ZRE). This is what makes UniNet brokerless -
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
    # Probed unconditionally: libzyre.pc lists czmq/libzmq only as private
    # requirements, which pkg-config does not expand for a dynamic link (so a
    # zyre-only link fails with "DSO missing from command line"), AND the
    # from-source fallback below needs to know which of them it can reuse.
    pkg_check_modules(CZMQ IMPORTED_TARGET libczmq)
    pkg_check_modules(ZMQ  IMPORTED_TARGET libzmq)
    if(ZYRE_FOUND)
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
# Add a fetched dependency WITHOUT building anything we do not link against.
#
# czmq and zyre ship auxiliary executables (czmq_selftest, zsp, zmakecert,
# test_randof, zyre_selftest, zpinger, perf_local, perf_remote). UniNet uses
# none of them, but a plain FetchContent_MakeAvailable builds them all, and they
# link against whatever their own build happened to detect: curl's transitive
# dependencies, OpenLDAP, and so on. On a machine missing any of those, the
# build fails on a tool nobody asked for.
#
# EXCLUDE_FROM_ALL makes every target in the subdirectory build only if
# something actually depends on it. We link zyre-static, which pulls
# czmq-static and libzmq-static; nothing else is ever compiled.
macro(uninet_add_dependency name)
    FetchContent_Declare(${name} ${ARGN})
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
        FetchContent_MakeAvailable(${name})
    else()
        # EXCLUDE_FROM_ALL only became a FetchContent_Declare option in 3.28.
        FetchContent_GetProperties(${name})
        if(NOT ${name}_POPULATED)
            FetchContent_Populate(${name})
            add_subdirectory(${${name}_SOURCE_DIR} ${${name}_BINARY_DIR} EXCLUDE_FROM_ALL)
        endif()
    endif()
endmacro()

if(NOT UNINET_ZYRE_TARGET)
    # Ubuntu and Debian do not package Zyre, so this is the normal path there.
    # libzmq and czmq usually ARE packaged; use them when present and fetch only
    # what is missing, which is both faster and far less fragile.
    include(FetchContent)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
        set(UNINET_FC_EXCLUDE EXCLUDE_FROM_ALL)
    else()
        set(UNINET_FC_EXCLUDE "")
    endif()
    # Belt and braces alongside EXCLUDE_FROM_ALL: these stop the dependencies
    # registering their own ctest entries, so `ctest` shows UniNet's tests only.
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(CZMQ_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(ZYRE_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED  OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC  ON  CACHE BOOL "" FORCE)
    set(WITH_DOC      OFF CACHE BOOL "" FORCE)
    # Drafts ON, unlike the other flags here, which exist to keep the build
    # small. zyre_set_advertised_endpoint is a draft API, and it is what lets a
    # node behind a port forward tell peers the address they can actually reach
    # it on: a USB-tethered headset, a container, a VPN. Building the dependency
    # without drafts makes that setting impossible to honour, and UniNet then
    # refuses the join rather than advertising the wrong address.
    set(ENABLE_DRAFTS ON CACHE BOOL "" FORCE)
    set(ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WITH_PERF_TOOL  OFF CACHE BOOL "" FORCE)
    set(WITH_LIBBSD     OFF CACHE BOOL "" FORCE)
    set(CZMQ_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(CZMQ_BUILD_STATIC ON  CACHE BOOL "" FORCE)
    set(ZYRE_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZYRE_BUILD_STATIC ON  CACHE BOOL "" FORCE)

    # czmq and zyre locate their dependencies with find_package(), which only
    # ever looks for an INSTALLED library. It cannot see a sibling target that
    # FetchContent built in the same tree, so each step below hands the next one
    # explicit paths. Without this the configure failed with "libzmq not found"
    # on any machine that did not already have libzmq installed, and appeared to
    # work on any machine that did.
    if(NOT ZMQ_FOUND)
        message(STATUS "UniNet: fetching libzmq.")
        uninet_add_dependency(libzmq
            GIT_REPOSITORY https://github.com/zeromq/libzmq.git GIT_TAG v4.3.5
            ${UNINET_FC_EXCLUDE})
        set(LIBZMQ_LIBRARIES    libzmq-static                    CACHE STRING "" FORCE)
        set(LIBZMQ_INCLUDE_DIRS "${libzmq_SOURCE_DIR}/include"   CACHE STRING "" FORCE)
        set(UNINET_FETCHED_ZMQ libzmq-static)
    else()
        message(STATUS "UniNet: using system libzmq ${ZMQ_VERSION}.")
    endif()

    if(NOT CZMQ_FOUND)
        message(STATUS "UniNet: fetching czmq.")
        uninet_add_dependency(czmq
            GIT_REPOSITORY https://github.com/zeromq/czmq.git GIT_TAG v4.2.1
            ${UNINET_FC_EXCLUDE})
        set(CZMQ_LIBRARIES    czmq-static                      CACHE STRING "" FORCE)
        set(CZMQ_INCLUDE_DIRS "${czmq_SOURCE_DIR}/include"     CACHE STRING "" FORCE)
        set(UNINET_FETCHED_CZMQ czmq-static)
    else()
        message(STATUS "UniNet: using system czmq ${CZMQ_VERSION}.")
    endif()

    message(STATUS "UniNet: fetching zyre.")
    uninet_add_dependency(zyre
        GIT_REPOSITORY https://github.com/zeromq/zyre.git GIT_TAG v2.0.1
        ${UNINET_FC_EXCLUDE})

    # Link what was fetched plus whatever came from the system.
    set(UNINET_ZYRE_TARGET zyre-static)
    if(UNINET_FETCHED_CZMQ)
        list(APPEND UNINET_ZYRE_TARGET ${UNINET_FETCHED_CZMQ})
    elseif(CZMQ_FOUND)
        list(APPEND UNINET_ZYRE_TARGET PkgConfig::CZMQ)
    endif()
    if(UNINET_FETCHED_ZMQ)
        list(APPEND UNINET_ZYRE_TARGET ${UNINET_FETCHED_ZMQ})
    elseif(ZMQ_FOUND)
        list(APPEND UNINET_ZYRE_TARGET PkgConfig::ZMQ)
    endif()
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
if(ANDROID)
    # czmq logs through __android_log_print, so the NDK's liblog is required.
    # Without it the link fails with an undefined symbol that names czmq rather
    # than the missing library, which is not an obvious diagnosis.
    find_library(ANDROID_LOG_LIB log)
    target_link_libraries(uninet PUBLIC ${ANDROID_LOG_LIB})
endif()

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
# Labelled so `ctest -L uninet` runs OUR tests only. When the dependencies are
# built from source they register their own, and a bare ctest then runs 35
# tests instead of 3, most of them upstream's.
set_tests_properties(roundtrip PROPERTIES LABELS uninet)

add_executable(test_network tests/test_network.cpp)
target_link_libraries(test_network PRIVATE uninet uninet_warnings)
add_test(NAME network COMMAND test_network)
# Two nodes must find each other over a real UDP beacon; give it room on a
# loaded CI box before calling it a failure.
set_tests_properties(network PROPERTIES TIMEOUT 90 LABELS uninet)

# ── Benchmarks ──
# Two, with names that say which is which: the codec in isolation, and what a
# user actually experiences (two real nodes over the network).
add_executable(benchmark_codec tests/benchmark_codec.cpp)
target_link_libraries(benchmark_codec PRIVATE uninet uninet_warnings)
set_target_properties(benchmark_codec PROPERTIES OUTPUT_NAME uninet-benchmark-codec)

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
# On by default: it needs no dependency beyond what the core already links, and
# leaving it off meant a fresh clone produced no library for C#/Unity and no way
# to run the C ABI test, which is the exact path those bindings take.
option(UNINET_BUILD_CABI "Build the C ABI shared library (libuninet_c)" ON)
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
    set_tests_properties(cabi PROPERTIES TIMEOUT 90 LABELS uninet)
endif()

# ── Python extension (pybind11). `pip install` drives it via scikit-build-core.
option(UNINET_BUILD_PYTHON "Build the pybind11 Python extension (_uninet)" ON)
if(UNINET_BUILD_PYTHON)
    # QUIET, not REQUIRED, because this is now on by default: a C++-only
    # consumer without pybind11 installed must still get a working build rather
    # than a hard configure error for a component they never asked for.
    find_package(pybind11 CONFIG QUIET)
    if(NOT pybind11_FOUND)
        # pybind11 is distributed as a Python package, so its CMake config sits
        # inside site-packages, which is not on any prefix find_package searches.
        # Almost nobody has it anywhere else, so plain find_package misses it on
        # a machine that does have pybind11 installed, and the extension gets
        # skipped for no reason. Ask the interpreter instead, which is what the
        # bootstrap script always did by hand.
        # Development.Module too, not just Interpreter: pybind11's newer CMake
        # calls python3_add_library(), which only exists once that component has
        # been found. With Interpreter alone the configure fails with
        # "Unknown CMake command python3_add_library", which reads like a broken
        # pybind11 rather than a missing python3-dev.
        find_package(Python3 COMPONENTS Interpreter Development.Module QUIET)
        if(Python3_Interpreter_FOUND AND Python3_Development.Module_FOUND)
            execute_process(
                COMMAND "${Python3_EXECUTABLE}" -m pybind11 --cmakedir
                OUTPUT_VARIABLE UNINET_PYBIND11_DIR
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE UNINET_PYBIND11_RC
                ERROR_QUIET)
            if(UNINET_PYBIND11_RC EQUAL 0 AND EXISTS "${UNINET_PYBIND11_DIR}")
                find_package(pybind11 CONFIG QUIET
                             PATHS "${UNINET_PYBIND11_DIR}" NO_DEFAULT_PATH)
            endif()
        endif()
    endif()
endif()
if(UNINET_BUILD_PYTHON AND NOT pybind11_FOUND)
    if(DEFINED SKBUILD)
        # Under pip there is no "carry on without it": the whole point of the
        # build is the extension, and continuing produces a wheel containing
        # __init__.py and nothing to import, which fails at runtime on the
        # user's machine with ModuleNotFoundError: uninet._uninet. Fail here,
        # where the message can still say what to do.
        message(FATAL_ERROR
            "pybind11 is required to build the Python extension but was not found.\n"
            "Install it with:  pip install pybind11")
    endif()
    message(STATUS "UniNet: pybind11 not found, skipping the Python extension.")
    set(UNINET_BUILD_PYTHON OFF)
endif()
if(UNINET_BUILD_PYTHON)
    pybind11_add_module(_uninet python/bindings.cpp)
    target_link_libraries(_uninet PRIVATE uninet)
    if(NOT DEFINED SKBUILD)
        # Plain cmake build: drop the extension next to __init__.py so that
        # `PYTHONPATH=python python -c "import uninet"` works with no install
        # step. Skipped under pip, which must not write into the source tree.
        set_target_properties(_uninet PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/python/uninet")
    endif()
    # This is what puts the extension INTO THE WHEEL. scikit-build-core packages
    # what CMake installs, and there was no install rule for _uninet: only
    # LIBRARY_OUTPUT_DIRECTORY, which is a build-tree setting, not an install
    # one. So `pip install .` produced a package containing __init__.py and no
    # extension, and the first line of __init__.py then failed with
    #     ImportError: cannot import name '_uninet' from 'uninet'
    # The in-tree tests never caught it because they import from python/, where
    # the line above had already put the file.
    install(TARGETS _uninet LIBRARY DESTINATION uninet)
endif()

# ── Install rules, so consumers can find_package(UniNet) instead of vendoring
# a file copy (which is how ThermoNavServer ended up with a diverging fork).
include(GNUInstallDirs)
# No install(EXPORT)/find_package support on purpose. `uninet` links PUBLIC to
# either PkgConfig::ZYRE or a fetched zyre-static, and neither is exportable:
# the imported pkg-config target does not survive an export set, and a
# FetchContent static target cannot be installed at all. Shipping a
# UniNetConfig.cmake that works only in the system-zyre case would be worse than
# not shipping one. add_subdirectory() is the supported integration; the README
# says so.
install(TARGETS uninet
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
