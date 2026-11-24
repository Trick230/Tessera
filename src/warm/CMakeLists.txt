# SPDX-License-Identifier: GPL-3.0-or-later
# tessera_warm.exe (S5): out-of-process shader warm-up, run by the app before
# the game. Separate exe so a driver crash never takes the app down.
add_executable(tessera_warm main.cpp)
target_include_directories(tessera_warm PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(tessera_warm PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(tessera_warm PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
target_link_libraries(tessera_warm PRIVATE d3d11 d3d12 dxgi d3dcompiler dxguid)
