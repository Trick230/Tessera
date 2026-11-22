# SPDX-License-Identifier: GPL-3.0-or-later
include(FetchContent)

# Keep the SDK-only layer independent of the offline codecs and metric.
set(BUILD_TOOLS OFF)
set(BUILD_SAMPLE OFF)
set(BUILD_DX11 OFF)
set(BUILD_DX12 OFF)
set(BC_USE_OPENMP OFF)
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

add_executable(tessera_texcomp main.cpp)
target_include_directories(tessera_texcomp PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_include_directories(tessera_texcomp SYSTEM PRIVATE "${tessera_flip_SOURCE_DIR}/src/cpp")
target_compile_options(tessera_texcomp PRIVATE /EHsc)
target_link_libraries(tessera_texcomp PRIVATE DirectXTex)
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
