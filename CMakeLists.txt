cmake_minimum_required(VERSION 3.25)

project(dcb_recomp
    VERSION 0.1.0
    DESCRIPTION "Static recompilation of Digimon Card Battle (PS1) to native code"
    LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Use a preset: cmake --preset linux-debug")
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
set(DCB_GAME_ID "SLPS-03101" CACHE STRING "Disc serial being recompiled; selects config/<id> and generated/<id>")
set_property(CACHE DCB_GAME_ID PROPERTY STRINGS SLPS-03101 SLUS-01328)
option(DCB_BUILD_TOOLS "Build host tools (MIPS->C recompiler). Skipped automatically when cross-compiling." ON)
option(DCB_BUILD_TESTS "Build unit tests" ON)
option(DCB_USE_SDL3    "Build the SDL3 platform backend (window, audio, input)" OFF)

# ---------------------------------------------------------------------------
# Language standards: generated code is C, everything hand-written is C++.
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(DcbCompilerOptions)   # dcb::warnings, dcb::generated_code

# ---------------------------------------------------------------------------
# psx_runtime — CPU context, memory bus, indirect-call dispatch, GTE.
# The only layer generated code talks to directly.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE PSX_RUNTIME_SOURCES CONFIGURE_DEPENDS src/runtime/src/*.c src/runtime/src/*.cpp)
add_library(psx_runtime STATIC ${PSX_RUNTIME_SOURCES})
target_include_directories(psx_runtime PUBLIC src/runtime/include)
target_link_libraries(psx_runtime PRIVATE dcb::warnings)

# ---------------------------------------------------------------------------
# psx_hle — native replacements: BIOS call tables, Psy-Q libraries,
# and the hardware-register (MMIO) fallback for direct pokes.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE PSX_HLE_SOURCES CONFIGURE_DEPENDS src/hle/*.c src/hle/*.cpp)
add_library(psx_hle STATIC ${PSX_HLE_SOURCES})
target_include_directories(psx_hle PUBLIC src/hle)
target_link_libraries(psx_hle PUBLIC psx_runtime PRIVATE dcb::warnings)

# ---------------------------------------------------------------------------
# dcb_platform — host window / audio / input.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE DCB_PLATFORM_SOURCES CONFIGURE_DEPENDS src/platform/*.cpp)
add_library(dcb_platform STATIC ${DCB_PLATFORM_SOURCES})
target_include_directories(dcb_platform PUBLIC src/platform)
target_link_libraries(dcb_platform PUBLIC psx_hle PRIVATE dcb::warnings)
if(DCB_USE_SDL3)
    find_package(SDL3 CONFIG QUIET)
    if(NOT SDL3_FOUND)
        include(FetchContent)
        FetchContent_Declare(SDL3
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG        release-3.2.24
            GIT_SHALLOW    TRUE)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON  CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(SDL3)
    endif()
    target_link_libraries(dcb_platform PUBLIC SDL3::SDL3)
    target_compile_definitions(dcb_platform PUBLIC DCB_HAS_SDL3=1)
endif()

# ---------------------------------------------------------------------------
# dcb_recompiled — MIPS->C output of tools/recomp (gitignored, never hand-edited).
# Until the recompiler has run, an empty function table keeps the build linking.
# ---------------------------------------------------------------------------
set(DCB_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated/${DCB_GAME_ID}")
file(GLOB_RECURSE DCB_GENERATED_SOURCES CONFIGURE_DEPENDS "${DCB_GENERATED_DIR}/*.c")
if(NOT DCB_GENERATED_SOURCES)
    message(STATUS "No generated code in ${DCB_GENERATED_DIR}; linking the empty function table")
    set(DCB_GENERATED_SOURCES src/runtime/stub/empty_function_table.c)
endif()
add_library(dcb_recompiled STATIC ${DCB_GENERATED_SOURCES})
target_link_libraries(dcb_recompiled PRIVATE psx_runtime dcb::generated_code)

# ---------------------------------------------------------------------------
# dcb — the game executable. src/game holds hand-written overrides that
# replace individual recompiled functions (fixes, enhancements, mods).
# ---------------------------------------------------------------------------
file(GLOB_RECURSE DCB_GAME_SOURCES CONFIGURE_DEPENDS src/game/*.c src/game/*.cpp)
add_executable(dcb ${DCB_GAME_SOURCES})
target_link_libraries(dcb PRIVATE dcb_recompiled dcb_platform psx_hle psx_runtime dcb::warnings)
target_compile_definitions(dcb PRIVATE DCB_GAME_ID="${DCB_GAME_ID}")
if(MINGW)
    target_link_options(dcb PRIVATE -static)   # no libstdc++/libgcc DLLs next to the .exe
endif()

# ---------------------------------------------------------------------------
# Host tools: must run on the build machine, so skip them in cross builds.
# ---------------------------------------------------------------------------
if(DCB_BUILD_TOOLS AND NOT CMAKE_CROSSCOMPILING)
    file(GLOB_RECURSE DCB_RECOMP_SOURCES CONFIGURE_DEPENDS tools/recomp/src/*.cpp)
    add_executable(dcb_recompiler ${DCB_RECOMP_SOURCES})
    target_link_libraries(dcb_recompiler PRIVATE dcb::warnings)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(DCB_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_executable(test_runtime_memory tests/runtime/test_memory.cpp)
    target_link_libraries(test_runtime_memory PRIVATE psx_runtime dcb::warnings)
    add_test(NAME runtime.memory COMMAND test_runtime_memory)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME tools.extract_disc
                 COMMAND Python3::Interpreter -m unittest discover -s tools/disc/tests -v
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
endif()
