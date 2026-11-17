cmake_minimum_required(VERSION 3.24)

project(ACVUpscaler LANGUAGES CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ACVUpscaler is a 64-bit ReShade add-on and needs the MSVC x64 toolchain (run tools/build.cmd).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: the add-on is loaded into the game process and must not depend on a VC++ redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ReShade 6.8.0 (RESHADE_API_VERSION 20) and the Dear ImGui 1.92.5 headers it expects
set(RESHADE_DIR "${CMAKE_SOURCE_DIR}/external/reshade")
if(NOT EXISTS "${RESHADE_DIR}/include/reshade.hpp")
    message(FATAL_ERROR "ReShade sources not found in ${RESHADE_DIR} (run tools/fetch_deps.cmd).")
endif()

set(FFX_DIR "${CMAKE_SOURCE_DIR}/external/FidelityFX-SDK/Kits/FidelityFX")
if(NOT EXISTS "${FFX_DIR}/signedbin/amd_fidelityfx_loader_dx12.dll")
    message(FATAL_ERROR "FidelityFX SDK not found in ${FFX_DIR} (run tools/fetch_deps.cmd).")
endif()

# NVIDIA DLSS SDK 310.9.16 (NGX headers and static library), optional. Without it the add-on is built without its
# own DLSS path and Upscaler=DLSS falls back to FSR 3.1.
set(DLSS_DIR "${CMAKE_SOURCE_DIR}/external/DLSS")
set(DLSS_LIB "${DLSS_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib") # Built for the static CRT, like the add-on
if(EXISTS "${DLSS_DIR}/include/nvsdk_ngx.h" AND EXISTS "${DLSS_LIB}")
    option(ACV_WITH_DLSS "Build the DLSS path (NVIDIA NGX SDK)" ON)
else()
    message(STATUS "NVIDIA DLSS SDK not found in ${DLSS_DIR}: the add-on is built without DLSS (run tools/fetch_deps.cmd).")
    set(ACV_WITH_DLSS OFF CACHE BOOL "Build the DLSS path (NVIDIA NGX SDK)" FORCE)
endif()

# NVIDIA NVAPI R620 (header and nvapi64.lib, MIT), optional. Without it the add-on is built without Reflex.
set(NVAPI_DIR "${CMAKE_SOURCE_DIR}/external/nvapi")
if(EXISTS "${NVAPI_DIR}/nvapi.h" AND EXISTS "${NVAPI_DIR}/amd64/nvapi64.lib")
    option(ACV_WITH_REFLEX "Build the Reflex low latency mode (NVIDIA NVAPI)" ON)
else()
    message(STATUS "NVIDIA NVAPI not found in ${NVAPI_DIR}: the add-on is built without Reflex (run tools/fetch_deps.cmd).")
    set(ACV_WITH_REFLEX OFF CACHE BOOL "Build the Reflex low latency mode (NVIDIA NVAPI)" FORCE)
endif()

add_library(ACVUpscaler SHARED
    src/addon.cpp
    src/autotest.cpp
    src/descriptors.cpp
    src/dlss.cpp
    src/fsr.cpp
    src/frame.cpp
    src/overlay.cpp
    src/pipelines.cpp
    src/record.cpp
    src/reflex.cpp
    src/replace.cpp
    src/settings.cpp
    src/snapshot.cpp
    src/autotest.hpp
    src/common.hpp
    src/descriptors.hpp
    src/dlss.hpp
    src/dxbc.hpp
    src/frame.hpp
    src/fsr.hpp
    src/overlay.hpp
    src/pipelines.hpp
    src/reflex.hpp
    src/replace.hpp
    src/snapshot.hpp)

target_include_directories(ACVUpscaler SYSTEM PRIVATE
    "${RESHADE_DIR}/include"
    "${RESHADE_DIR}/deps/imgui"
    "${FFX_DIR}/api/include"
    "${FFX_DIR}/upscalers/include")

target_link_libraries(ACVUpscaler PRIVATE d3d12 d3dcompiler)

if(ACV_WITH_DLSS)
    target_include_directories(ACVUpscaler SYSTEM PRIVATE "${DLSS_DIR}/include")
    target_link_libraries(ACVUpscaler PRIVATE "${DLSS_LIB}")
    target_compile_definitions(ACVUpscaler PRIVATE ACV_WITH_DLSS)
endif()

if(ACV_WITH_REFLEX)
    target_include_directories(ACVUpscaler SYSTEM PRIVATE "${NVAPI_DIR}")
    target_link_libraries(ACVUpscaler PRIVATE "${NVAPI_DIR}/amd64/nvapi64.lib")
    target_compile_definitions(ACVUpscaler PRIVATE ACV_WITH_REFLEX)
endif()

target_compile_definitions(ACVUpscaler PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    ImTextureID=ImU64) # Required by reshade_overlay.hpp, has to be set before imgui.h is included

target_compile_options(ACVUpscaler PRIVATE /W4 /permissive- /utf-8)
# /PDBALTPATH: the binary names its debug file without the build machine's folder
target_link_options(ACVUpscaler PRIVATE /INCREMENTAL:NO /OPT:REF /OPT:ICF /PDBALTPATH:ACVUpscaler.pdb)

# ReShade loads add-ons from <project>/build/bin (ReShade.ini, [ADDON] AddonPath)
set_target_properties(ACVUpscaler PROPERTIES
    PREFIX ""
    OUTPUT_NAME "ACVUpscaler"
    SUFFIX ".addon64"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/bin"
    PDB_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/bin")

# AMD's signed FidelityFX DLLs go next to the add-on (never into the game folder); fsr.cpp loads them by full path
add_custom_command(TARGET ACVUpscaler POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${FFX_DIR}/signedbin/amd_fidelityfx_loader_dx12.dll"
        "${FFX_DIR}/signedbin/amd_fidelityfx_upscaler_dx12.dll"
        "${CMAKE_SOURCE_DIR}/build/bin")

# Offline test of the checksum extraction against two shader blobs extracted from frame captures of the game.
# The blobs are game content and are not part of the repository: without them the test is not registered.
add_executable(checksum_test tests/checksum_test.cpp src/dxbc.hpp)
target_compile_options(checksum_test PRIVATE /W4 /permissive- /utf-8)

enable_testing()
set(CHECKSUM_TEST_PS "${CMAKE_SOURCE_DIR}/captures/A/shaders/pso22536_ps.bin")
set(CHECKSUM_TEST_CS "${CMAKE_SOURCE_DIR}/captures/B/shaders/pso22376_cs.bin")
if(EXISTS "${CHECKSUM_TEST_PS}" AND EXISTS "${CHECKSUM_TEST_CS}")
    add_test(NAME dxbc_checksum COMMAND checksum_test "${CHECKSUM_TEST_PS}" "${CHECKSUM_TEST_CS}")
else()
    message(STATUS "Shader blobs for the checksum test not found: the test is skipped.")
endif()
