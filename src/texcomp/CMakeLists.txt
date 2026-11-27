# SPDX-License-Identifier: GPL-3.0-or-later
include(FetchContent)

# Keep the SDK-only layer independent of the offline codecs and metric. BUILD_DX11 brings
# DirectXTex's DirectCompute BC6H/BC7 encoder; its shaders are compiled below by our own
# tool instead of fxc.exe (not in the portable toolchain).
set(BUILD_TOOLS OFF)
set(BUILD_SAMPLE OFF)
set(BUILD_DX11 ON)
set(BUILD_DX12 OFF)
set(BC_USE_OPENMP OFF)
set(USE_PREBUILT_SHADERS ON)
set(COMPILED_SHADERS "${CMAKE_CURRENT_BINARY_DIR}/directxtex-shaders")
FetchContent_Declare(tessera_directxtex
  URL https://codeload.github.com/microsoft/DirectXTex/tar.gz/74f9b02475b5c52b5f45cb9f8086aadf32c766be
  URL_HASH SHA256=c402cdf6d5cedef51be1ed16aaaa3516880bf17bb4546e98e9a7c0ac7d6dfa42
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_Declare(tessera_flip
  URL https://codeload.github.com/NVlabs/flip/tar.gz/d3ab02a4425eb167d8a4021b382a2bafc6e6c838
  URL_HASH SHA256=991abec79184de1f983f303c34d97cd64ca463c24f74c42f6285d5bfc68af42e
  SOURCE_SUBDIR tessera-header-only
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(tessera_directxtex tessera_flip)
target_compile_options(DirectXTex PRIVATE /EHsc)

# The DirectCompute shaders as headers, cs_5_0 and the cs_4_0 fallback (what CompileShaders.cmd
# makes with fxc). The tool runs on the build host, so this needs a Windows host.
if(NOT CMAKE_HOST_WIN32)
  message(FATAL_ERROR "tessera_texcomp needs a Windows build host (its shader tool runs during the build)")
endif()
add_executable(tessera_hlsl_header hlsl_header.cpp)
target_compile_options(tessera_hlsl_header PRIVATE /EHsc)
target_link_libraries(tessera_hlsl_header PRIVATE d3dcompiler)
set(shader_dir "${tessera_directxtex_SOURCE_DIR}/DirectXTex/Shaders")
set(shader_headers)
foreach(shader BC7Encode:TryMode456CS BC7Encode:TryMode137CS BC7Encode:TryMode02CS BC7Encode:EncodeBlockCS
               BC6HEncode:TryModeG10CS BC6HEncode:TryModeLE10CS BC6HEncode:EncodeBlockCS)
  string(REPLACE ":" ";" parts "${shader}")
  list(GET parts 0 file)
  list(GET parts 1 entry)
  set(name "${file}_${entry}")
  add_custom_command(OUTPUT "${COMPILED_SHADERS}/${name}.inc" "${COMPILED_SHADERS}/${name}_cs40.inc"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${COMPILED_SHADERS}"
    COMMAND $<TARGET_FILE:tessera_hlsl_header> "${shader_dir}/${file}.hlsl" ${entry} cs_5_0 ${name} "${COMPILED_SHADERS}/${name}.inc"
    COMMAND $<TARGET_FILE:tessera_hlsl_header> "${shader_dir}/${file}.hlsl" ${entry} cs_4_0 ${name}
            "${COMPILED_SHADERS}/${name}_cs40.inc" EMULATE_F16C
    DEPENDS tessera_hlsl_header "${shader_dir}/${file}.hlsl"
    VERBATIM)
  list(APPEND shader_headers "${COMPILED_SHADERS}/${name}.inc" "${COMPILED_SHADERS}/${name}_cs40.inc")
endforeach()
add_custom_target(tessera_directxtex_shaders DEPENDS ${shader_headers})
set_source_files_properties(${shader_headers} TARGET_DIRECTORY DirectXTex PROPERTIES GENERATED ON)
add_dependencies(DirectXTex tessera_directxtex_shaders)

add_executable(tessera_texcomp main.cpp)
target_include_directories(tessera_texcomp PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_include_directories(tessera_texcomp SYSTEM PRIVATE "${tessera_flip_SOURCE_DIR}/src/cpp")
target_compile_options(tessera_texcomp PRIVATE /EHsc)
target_link_libraries(tessera_texcomp PRIVATE DirectXTex d3d11)
add_custom_command(TARGET tessera_texcomp POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${tessera_directxtex_SOURCE_DIR}/LICENSE"
          "$<TARGET_FILE_DIR:tessera_texcomp>/DirectXTex-LICENSE.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${tessera_flip_SOURCE_DIR}/LICENSE"
          "$<TARGET_FILE_DIR:tessera_texcomp>/FLIP-LICENSE.txt")

add_executable(tessera_test_feedback EXCLUDE_FROM_ALL ../../tests/quality/feedback.cpp)
target_compile_options(tessera_test_feedback PRIVATE /EHsc)
target_link_libraries(tessera_test_feedback PRIVATE d3d11 d3dcompiler)
add_library(tessera_test_foreign_draw SHARED EXCLUDE_FROM_ALL ../../tests/quality/foreign_draw.cpp)
add_dependencies(tessera_test_feedback tessera_test_foreign_draw)
