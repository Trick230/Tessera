# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)
project(Tessera VERSION 0.1.0 LANGUAGES C CXX ASM_MASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Exception model is chosen per target (the layer builds without exceptions).
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
# Static CRT: the layer must not depend on a VC++ redistributable in the game folder.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Release: ThinLTO and unused data dropped (/Gw).
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
add_compile_options($<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/Gw>)

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/W4> $<$<COMPILE_LANGUAGE:C,CXX>:/permissive->
                    $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8> $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>)

add_subdirectory(src/dll)
add_subdirectory(src/warm)
add_subdirectory(tests/d3d11_smoke)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  add_subdirectory(src/d3d12_capture)
endif()

option(TESSERA_BUILD_VULKAN_TELEMETRY "Build the optional Vulkan telemetry layer" OFF)
if(TESSERA_BUILD_VULKAN_TELEMETRY)
  add_subdirectory(src/vulkan_telemetry)
endif()

option(TESSERA_BUILD_QUALITY "Build the offline FLIP texture tool" OFF)
if(TESSERA_BUILD_QUALITY)
  add_subdirectory(src/texcomp)
endif()
