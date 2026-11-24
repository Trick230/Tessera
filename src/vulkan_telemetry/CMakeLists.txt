# SPDX-License-Identifier: GPL-3.0-or-later
find_path(TESSERA_VULKAN_HEADERS vulkan/vk_layer.h HINTS ENV VULKAN_SDK PATH_SUFFIXES Include REQUIRED)
add_library(tessera_vulkan_telemetry SHARED layer.cpp exports.def)
target_include_directories(tessera_vulkan_telemetry PRIVATE ${TESSERA_VULKAN_HEADERS})
target_compile_definitions(tessera_vulkan_telemetry PRIVATE VK_NO_PROTOTYPES)
target_compile_options(tessera_vulkan_telemetry PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
add_executable(tessera_vulkan_replay replay.cpp)
target_include_directories(tessera_vulkan_replay PRIVATE ${TESSERA_VULKAN_HEADERS})
target_compile_definitions(tessera_vulkan_replay PRIVATE VK_NO_PROTOTYPES)
target_compile_options(tessera_vulkan_replay PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
add_custom_command(TARGET tessera_vulkan_telemetry POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/TesseraTelemetry.json"
          "$<TARGET_FILE_DIR:tessera_vulkan_telemetry>/TesseraTelemetry.json")
