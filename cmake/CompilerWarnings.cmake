# Centralised warning configuration.
#
# Warnings are attached to an INTERFACE target rather than added globally so
# that third-party code pulled in via FetchContent is not held to the same
# standard as our own sources (we cannot fix warnings we do not own).

add_library(testforge_warnings INTERFACE)
add_library(TestForge::warnings ALIAS testforge_warnings)

set(_tf_gcc_clang_warnings
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow                # a shadowed variable is nearly always a bug
    -Wnon-virtual-dtor      # polymorphic deletion through a base pointer
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wconversion
    -Wsign-conversion
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    -Wnull-dereference
)

set(_tf_gcc_only_warnings
    -Wduplicated-cond
    -Wduplicated-branches
    -Wlogical-op
    -Wuseless-cast
)

set(_tf_msvc_warnings /W4 /permissive-)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    target_compile_options(testforge_warnings INTERFACE
        ${_tf_gcc_clang_warnings} ${_tf_gcc_only_warnings})
    if(TESTFORGE_WERROR)
        target_compile_options(testforge_warnings INTERFACE -Werror)
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(testforge_warnings INTERFACE ${_tf_gcc_clang_warnings})
    if(TESTFORGE_WERROR)
        target_compile_options(testforge_warnings INTERFACE -Werror)
    endif()
elseif(MSVC)
    target_compile_options(testforge_warnings INTERFACE ${_tf_msvc_warnings})
    if(TESTFORGE_WERROR)
        target_compile_options(testforge_warnings INTERFACE /WX)
    endif()
endif()
