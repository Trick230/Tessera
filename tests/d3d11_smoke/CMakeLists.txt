# SPDX-License-Identifier: GPL-3.0-or-later
# Runs a tiny D3D11 app with the layer next to it as d3d11.dll (app-folder
# DLLs win over System32 for d3d11, which is not a KnownDLL).
add_executable(d3d11_smoke main.cpp)
target_include_directories(d3d11_smoke PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(d3d11_smoke PRIVATE d3d11 dxgi d3dcompiler)
target_compile_options(d3d11_smoke PRIVATE /EHsc)
add_dependencies(d3d11_smoke tessera_d3d11)
add_custom_command(TARGET d3d11_smoke POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:tessera_d3d11> $<TARGET_FILE_DIR:d3d11_smoke>/d3d11.dll)
add_library(nvapi_probe SHARED nvapi_probe.cpp)
add_dependencies(d3d11_smoke nvapi_probe)
