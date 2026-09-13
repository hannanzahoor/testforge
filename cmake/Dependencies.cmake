# Third-party dependency acquisition.
#
# Strategy: CMake FetchContent (see docs/design-decisions.md).
#   * no package manager to install before the first build
#   * exact versions pinned in-tree, so CI and a laptop build the same thing
#   * dependencies are cached under ${TESTFORGE_DEPS_CACHE} for offline builds
#
# Deliberately NOT dependencies: libcurl, OpenSSL and nlohmann/json. TestForge
# ships its own JSON parser and POSIX HTTP client, which keeps the build
# self-contained on machines without development headers installed.

include(FetchContent)

set(TESTFORGE_DEPS_CACHE "${CMAKE_SOURCE_DIR}/.deps-cache"
    CACHE PATH "Directory used to cache downloaded dependency archives")
set(FETCHCONTENT_BASE_DIR "${TESTFORGE_DEPS_CACHE}" CACHE PATH "" FORCE)
set(FETCHCONTENT_QUIET OFF)

option(TESTFORGE_USE_SYSTEM_SQLITE "Link against a system-provided SQLite3" OFF)

# ---------------------------------------------------------------------------
# SQLite3 (amalgamation)
# ---------------------------------------------------------------------------
if(TESTFORGE_USE_SYSTEM_SQLITE)
    find_package(SQLite3 REQUIRED)
    add_library(testforge_sqlite3 INTERFACE)
    target_link_libraries(testforge_sqlite3 INTERFACE SQLite::SQLite3)
    message(STATUS "SQLite3: system (${SQLite3_VERSION})")
else()
    # DOWNLOAD_EXTRACT_TIMESTAMP arrived in CMake 3.24, together with policy
    # CMP0135. Passing it unconditionally breaks every older CMake, and it
    # breaks it in a way that points at the wrong line: FetchContent forwards
    # these arguments to ExternalProject_Add, whose parser only knows the
    # keywords of the running version. An unrecognised keyword is not an
    # error there -- it is absorbed as a further *value* of the preceding
    # one-value keyword. URL_HASH therefore became the three-element list
    #
    #     SHA256=5592243...;DOWNLOAD_EXTRACT_TIMESTAMP;TRUE
    #
    # and failed the ALGO=value check. Pass the argument only where it exists,
    # so the declared 3.20 minimum is real.
    set(_testforge_fetch_options "")
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
        list(APPEND _testforge_fetch_options DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    endif()

    # SHA256 verified locally against the sqlite.org release archive.
    FetchContent_Declare(sqlite3_amalgamation
        URL      https://www.sqlite.org/2024/sqlite-amalgamation-3450100.zip
        URL_HASH SHA256=5592243caf28b2cdef41e6ab58d25d653dfc53deded8450eb66072c929f030c4
        ${_testforge_fetch_options})

    FetchContent_MakeAvailable(sqlite3_amalgamation)

    add_library(testforge_sqlite3 STATIC
        "${sqlite3_amalgamation_SOURCE_DIR}/sqlite3.c")
    target_include_directories(testforge_sqlite3 SYSTEM PUBLIC
        "${sqlite3_amalgamation_SOURCE_DIR}")

    # Compile-time configuration: drop features TestForge never uses, and
    # enable the ones the analytics queries rely on.
    target_compile_definitions(testforge_sqlite3 PUBLIC
        SQLITE_THREADSAFE=1              # serialized mode; we share one handle
        SQLITE_ENABLE_JSON1              # metadata columns are JSON documents
        SQLITE_DQS=0                     # reject double-quoted string literals
        SQLITE_DEFAULT_FOREIGN_KEYS=1
        SQLITE_OMIT_LOAD_EXTENSION       # no run-time extension loading
        SQLITE_OMIT_DEPRECATED)

    # Third-party C: silence warnings we are not going to fix.
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(testforge_sqlite3 PRIVATE -w)
    endif()
    set_target_properties(testforge_sqlite3 PROPERTIES
        C_CLANG_TIDY ""
        LINKER_LANGUAGE C)

    find_package(Threads REQUIRED)
    target_link_libraries(testforge_sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    message(STATUS "SQLite3: vendored amalgamation 3.45.1")
endif()

add_library(TestForge::sqlite3 ALIAS testforge_sqlite3)

# ---------------------------------------------------------------------------
# GoogleTest (only needed when building TestForge's own test-suite)
# ---------------------------------------------------------------------------
if(TESTFORGE_BUILD_TESTS)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        message(STATUS "GoogleTest: system installation")
    else()
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.14.0
            GIT_SHALLOW    TRUE)
        set(gtest_force_shared_crt ON  CACHE BOOL "" FORCE)
        set(INSTALL_GTEST          OFF CACHE BOOL "" FORCE)
        set(BUILD_GMOCK            ON  CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
        message(STATUS "GoogleTest: fetched v1.14.0")
    endif()
endif()
