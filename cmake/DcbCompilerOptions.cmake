# Shared compiler settings, exposed as INTERFACE targets.
#
#   dcb::warnings        strict warnings for hand-written code
#   dcb::generated_code  flags that make MIPS->C output behave like the R3000A

add_library(dcb_warnings INTERFACE)
add_library(dcb::warnings ALIAS dcb_warnings)

add_library(dcb_generated_code INTERFACE)
add_library(dcb::generated_code ALIAS dcb_generated_code)

if(MSVC)
    target_compile_options(dcb_warnings INTERFACE /W4 /permissive- /utf-8)
    # Recompiled functions are huge and numerous; warnings are noise there.
    target_compile_options(dcb_generated_code INTERFACE /W0 /bigobj /utf-8)
else()
    target_compile_options(dcb_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
    target_compile_options(dcb_generated_code INTERFACE
        -w
        # MIPS arithmetic wraps; C signed overflow is UB unless told otherwise.
        -fwrapv
        # Guest memory is accessed through casts of the RAM byte array.
        -fno-strict-aliasing)
    if(MINGW)
        target_compile_options(dcb_generated_code INTERFACE -Wa,-mbig-obj)
    endif()
endif()

# All guest memory access goes through a byte array and assumes a little-endian host.
include(TestBigEndian)
test_big_endian(DCB_HOST_BIG_ENDIAN)
if(DCB_HOST_BIG_ENDIAN)
    message(FATAL_ERROR "The runtime assumes a little-endian host (x86-64 / ARM64).")
endif()
