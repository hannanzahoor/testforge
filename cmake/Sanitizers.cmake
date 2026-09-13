# Sanitizer support.
#
# ASan and TSan are mutually exclusive; we fail configuration rather than
# emitting a link error that is much harder to interpret.

add_library(testforge_sanitizers INTERFACE)
add_library(TestForge::sanitizers ALIAS testforge_sanitizers)

if(TESTFORGE_SANITIZE_ADDRESS AND TESTFORGE_SANITIZE_THREAD)
    message(FATAL_ERROR
        "TESTFORGE_SANITIZE_ADDRESS and TESTFORGE_SANITIZE_THREAD cannot be "
        "enabled at the same time; run two separate builds.")
endif()

set(_tf_san_list "")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    if(TESTFORGE_SANITIZE_ADDRESS)
        list(APPEND _tf_san_list address leak)
    endif()
    if(TESTFORGE_SANITIZE_UB)
        list(APPEND _tf_san_list undefined)
    endif()
    if(TESTFORGE_SANITIZE_THREAD)
        list(APPEND _tf_san_list thread)
    endif()

    if(_tf_san_list)
        list(JOIN _tf_san_list "," _tf_san_flags)
        message(STATUS "Sanitizers enabled: ${_tf_san_flags}")
        target_compile_options(testforge_sanitizers INTERFACE
            -fsanitize=${_tf_san_flags}
            -fno-omit-frame-pointer
            -g)
        target_link_options(testforge_sanitizers INTERFACE
            -fsanitize=${_tf_san_flags})
    endif()
endif()
