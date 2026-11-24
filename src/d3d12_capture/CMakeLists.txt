# SPDX-License-Identifier: GPL-3.0-or-later
add_library(tessera_d3d12_capture SHARED
  proxy.cpp ../dll/vtable.cpp exports_d3d12.asm exports_d3d12.def)
target_include_directories(tessera_d3d12_capture PRIVATE ${PROJECT_SOURCE_DIR}/include ../dll)
target_compile_options(tessera_d3d12_capture PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
