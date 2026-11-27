# SPDX-License-Identifier: GPL-3.0-or-later
include(FetchContent)
FetchContent_Declare(tessera_vulkan_headers
  URL https://codeload.github.com/KhronosGroup/Vulkan-Headers/tar.gz/refs/tags/v1.4.321
  URL_HASH SHA256=7c1874f9186d9e7d5245e279cdccaa77a66ed8f52871b428630aeb771dc7623c
  SOURCE_SUBDIR tessera-header-only
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
FetchContent_MakeAvailable(tessera_vulkan_headers)
set(TESSERA_VULKAN_HEADERS "${tessera_vulkan_headers_SOURCE_DIR}/include")
add_library(tessera_vulkan_telemetry SHARED layer.cpp exports.def)
target_include_directories(tessera_vulkan_telemetry PRIVATE ${TESSERA_VULKAN_HEADERS} ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(tessera_vulkan_telemetry PRIVATE VK_NO_PROTOTYPES)
target_compile_options(tessera_vulkan_telemetry PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
add_executable(tessera_vulkan_replay replay.cpp)
target_include_directories(tessera_vulkan_replay PRIVATE ${TESSERA_VULKAN_HEADERS} ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(tessera_vulkan_replay PRIVATE VK_NO_PROTOTYPES)
target_compile_options(tessera_vulkan_replay PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
add_custom_command(TARGET tessera_vulkan_telemetry POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/TesseraTelemetry.json"
          "$<TARGET_FILE_DIR:tessera_vulkan_telemetry>/TesseraTelemetry.json")
add_executable(tessera_vulkan_device_fixture ${PROJECT_SOURCE_DIR}/tests/vulkan_device/main.cpp)
target_include_directories(tessera_vulkan_device_fixture PRIVATE ${TESSERA_VULKAN_HEADERS})
target_compile_definitions(tessera_vulkan_device_fixture PRIVATE VK_NO_PROTOTYPES)
# Frame limiter bench on Vulkan, run with tessera_vulkan.dll as vulkan-1.dll.
add_executable(tessera_vulkan_present ${PROJECT_SOURCE_DIR}/tests/vulkan_present/main.cpp)
target_include_directories(tessera_vulkan_present PRIVATE ${TESSERA_VULKAN_HEADERS})
target_compile_definitions(tessera_vulkan_present PRIVATE VK_NO_PROTOTYPES)
