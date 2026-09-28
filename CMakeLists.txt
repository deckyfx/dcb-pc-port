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
option(DCB_USE_SDL3    "Build the SDL3 platform backend (window, audio, input)" ON)

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
# dcb_vfs — asset pipeline runtime: TIM parsing, .pak archives, VFS mounts,
# PNG decoding (stb, vendored in third_party/stb). Linked into the game for
# HD texture replacement; the ripper reuses it offline.
# ---------------------------------------------------------------------------
file(GLOB DCB_VFS_SOURCES CONFIGURE_DEPENDS src/vfs/*.cpp)
add_library(dcb_vfs STATIC ${DCB_VFS_SOURCES})
target_include_directories(dcb_vfs PUBLIC src)
# SYSTEM keeps our strict warnings out of stb's headers.
target_include_directories(dcb_vfs SYSTEM PUBLIC third_party)
target_link_libraries(dcb_vfs PRIVATE dcb::warnings)

# ---------------------------------------------------------------------------
# psx_hle — native replacements: BIOS call tables, Psy-Q libraries,
# and the hardware-register (MMIO) fallback for direct pokes.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE PSX_HLE_SOURCES CONFIGURE_DEPENDS src/hle/*.c src/hle/*.cpp)
add_library(psx_hle STATIC ${PSX_HLE_SOURCES})
target_include_directories(psx_hle PUBLIC src/hle)
# PUBLIC: HdTextures (src/hle/gpu) is part of the interface via Gpu::install_hd().
target_link_libraries(psx_hle PUBLIC psx_runtime dcb_vfs PRIVATE dcb::warnings)

# ---------------------------------------------------------------------------
# dcb_platform — host window / audio / input.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE DCB_PLATFORM_SOURCES CONFIGURE_DEPENDS src/platform/*.cpp)
add_library(dcb_platform STATIC ${DCB_PLATFORM_SOURCES})
target_include_directories(dcb_platform PUBLIC src/platform)
target_link_libraries(dcb_platform PUBLIC psx_hle PRIVATE dcb::warnings)
if(DCB_USE_SDL3)
    # A system SDL3 is used when present; otherwise a pinned release is built statically so the
    # Windows .exe needs no SDL3.dll. SYSTEM keeps our strict warnings out of SDL's headers.
    find_package(SDL3 CONFIG QUIET)
    if(NOT SDL3_FOUND)
        include(FetchContent)
        FetchContent_Declare(SDL3
            URL      https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
            URL_HASH SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            SYSTEM)
        set(SDL_SHARED   OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC   ON  CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SDL_INSTALL  OFF CACHE BOOL "" FORCE)
        # Screensaver inhibition via libXss is not needed and its headers are often missing.
        set(SDL_X11_XSCRNSAVER OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(SDL3)
    endif()
    if(TARGET SDL3::SDL3-static)
        target_link_libraries(dcb_platform PUBLIC SDL3::SDL3-static)
    else()
        target_link_libraries(dcb_platform PUBLIC SDL3::SDL3)
    endif()
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
# Debug builds trap any `jr ra` that does not return to where its caller expects.
target_compile_definitions(dcb_recompiled PRIVATE $<$<CONFIG:Debug>:PSX_CHECK_RA>)

# ---------------------------------------------------------------------------
# dcb — the game executable. src/game holds hand-written overrides that
# replace individual recompiled functions (fixes, enhancements, mods).
# ---------------------------------------------------------------------------
file(GLOB_RECURSE DCB_GAME_SOURCES CONFIGURE_DEPENDS src/game/*.c src/game/*.cpp)
add_executable(dcb ${DCB_GAME_SOURCES})
target_link_libraries(dcb PRIVATE dcb_recompiled dcb_platform psx_hle psx_runtime dcb_vfs dcb::warnings)
target_compile_definitions(dcb PRIVATE DCB_GAME_ID="${DCB_GAME_ID}")
# Export symbols so the write watch (DCB_WATCH) can name the recompiled function that stored.
set_target_properties(dcb PROPERTIES ENABLE_EXPORTS ON)
# About page: version (git describe at configure time), build type, platform.
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --always --dirty --exclude "nightly*"
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} OUTPUT_VARIABLE DCB_VERSION
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(NOT DCB_VERSION)
    set(DCB_VERSION "unknown")
endif()
if(MINGW)
    set(DCB_PLATFORM_NAME "windows-x64-mingw")
elseif(WIN32)
    set(DCB_PLATFORM_NAME "windows-x64")
elseif(APPLE)
    set(DCB_PLATFORM_NAME "macos")
else()
    set(DCB_PLATFORM_NAME "linux")
endif()
target_compile_definitions(dcb PRIVATE DCB_VERSION_STRING="${DCB_VERSION}" DCB_BUILD_TYPE="$<CONFIG>"
                                       DCB_PLATFORM_NAME="${DCB_PLATFORM_NAME}")
if(MINGW)
    target_link_options(dcb PRIVATE -static)   # no libstdc++/libgcc DLLs next to the .exe
endif()

# ---------------------------------------------------------------------------
# Host tools: must run on the build machine, so skip them in cross builds.
# ---------------------------------------------------------------------------
if(DCB_BUILD_TOOLS AND NOT CMAKE_CROSSCOMPILING)
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
        URL      https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(nlohmann_json)

    file(GLOB DCB_RECOMP_CORE_SOURCES CONFIGURE_DEPENDS tools/recomp/src/*.cpp)
    list(REMOVE_ITEM DCB_RECOMP_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/tools/recomp/src/main.cpp)
    add_library(dcb_recomp_core STATIC ${DCB_RECOMP_CORE_SOURCES})
    target_include_directories(dcb_recomp_core PUBLIC tools/recomp/src)
    target_link_libraries(dcb_recomp_core PUBLIC nlohmann_json::nlohmann_json PRIVATE dcb::warnings)

    add_executable(dcb_recompiler tools/recomp/src/main.cpp)
    target_link_libraries(dcb_recompiler PRIVATE dcb_recomp_core dcb::warnings)

    # Offline asset pipeline: DRV unpacker + TIM->PNG converter + .pak packer.
    # Not part of any preset build dependency; run it explicitly when ripping assets.
    add_executable(dcb_asset_ripper tools/asset_ripper.cpp)
    target_link_libraries(dcb_asset_ripper PRIVATE dcb_vfs dcb::warnings)

    # `cmake --build --preset linux-debug --target recompile` regenerates generated/<id>/.
    add_custom_target(recompile
        COMMAND dcb_recompiler ${CMAKE_CURRENT_SOURCE_DIR} ${DCB_GAME_ID}
        COMMENT "Recompiling ${DCB_GAME_ID} into generated/${DCB_GAME_ID}"
        VERBATIM)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(DCB_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_executable(test_runtime_memory tests/runtime/test_memory.cpp)
    target_link_libraries(test_runtime_memory PRIVATE psx_runtime dcb::warnings)
    add_test(NAME runtime.memory COMMAND test_runtime_memory)

    add_executable(test_runtime_fiber tests/runtime/test_fiber.cpp)
    target_link_libraries(test_runtime_fiber PRIVATE psx_runtime dcb::warnings)
    add_test(NAME runtime.fiber COMMAND test_runtime_fiber)

    if(TARGET dcb_recomp_core)
        add_executable(test_recomp_decoder tests/recomp/test_decoder.cpp)
        target_link_libraries(test_recomp_decoder PRIVATE dcb_recomp_core dcb::warnings)
        add_test(NAME recomp.decoder COMMAND test_recomp_decoder)
    endif()

    # Each module registers its own tests in tests/<module>/tests.cmake (keeps parallel work
    # from colliding in this file).
    file(GLOB DCB_MODULE_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*/tests.cmake)
    foreach(module_tests IN LISTS DCB_MODULE_TESTS)
        include(${module_tests})
    endforeach()

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME tools.extract_disc
                 COMMAND Python3::Interpreter -m unittest discover -s tools/disc/tests -v
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME tools.coverage_diff
                 COMMAND Python3::Interpreter -m unittest discover -s tools/re/tests -v
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME tools.en_text
                 COMMAND Python3::Interpreter -m unittest discover -s tools/text/tests -v
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME tools.swap_us_images
                 COMMAND Python3::Interpreter -m unittest discover -s tools/assets/tests -v
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
endif()
