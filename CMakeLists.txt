cmake_minimum_required(VERSION 3.20)

project(TestForge
    VERSION 0.9.0
    DESCRIPTION "Intelligent C++ Test Automation & Validation Platform"
    # C is enabled only for the vendored SQLite amalgamation; every line of
    # TestForge itself is C++.
    LANGUAGES CXX C)

# ---------------------------------------------------------------------------
# Global project settings
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Position independent code keeps the option of a shared library open later.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(TESTFORGE_BUILD_TESTS      "Build TestForge's own test-suite"     ON)
option(TESTFORGE_BUILD_EXAMPLES   "Build the bundled example test-suites" ON)
option(TESTFORGE_BUILD_BENCHMARKS "Build the scheduling benchmarks"       ON)
option(TESTFORGE_WERROR           "Treat compiler warnings as errors"     OFF)
option(TESTFORGE_SANITIZE_ADDRESS "Enable AddressSanitizer + LeakSanitizer" OFF)
option(TESTFORGE_SANITIZE_UB      "Enable UndefinedBehaviorSanitizer"     OFF)
option(TESTFORGE_SANITIZE_THREAD  "Enable ThreadSanitizer"                OFF)
option(TESTFORGE_ENABLE_CLANG_TIDY "Run clang-tidy as part of the build"  OFF)

include(CompilerWarnings)
include(Sanitizers)
include(Dependencies)

# ---------------------------------------------------------------------------
# Threads
# ---------------------------------------------------------------------------
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(TESTFORGE_BUILD_EXAMPLES)
    # After src/: the example suites link against TestForge::core. The CLI
    # target picks their objects up via $<TARGET_OBJECTS:...>, which CMake
    # resolves at generate time, so declaration order here does not matter.
    add_subdirectory(examples)
endif()

if(TESTFORGE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(TESTFORGE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "==================== TestForge ${PROJECT_VERSION} ====================")
message(STATUS "  Build type ............ ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard .......... ${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler .............. ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Tests ................. ${TESTFORGE_BUILD_TESTS}")
message(STATUS "  Examples .............. ${TESTFORGE_BUILD_EXAMPLES}")
message(STATUS "  Benchmarks ............ ${TESTFORGE_BUILD_BENCHMARKS}")
message(STATUS "  ASan / UBSan / TSan ... ${TESTFORGE_SANITIZE_ADDRESS} / ${TESTFORGE_SANITIZE_UB} / ${TESTFORGE_SANITIZE_THREAD}")
message(STATUS "  Warnings as errors .... ${TESTFORGE_WERROR}")
message(STATUS "===========================================================")
message(STATUS "")
