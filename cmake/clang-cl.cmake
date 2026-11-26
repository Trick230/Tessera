# SPDX-License-Identifier: GPL-3.0-or-later
# Portable Windows toolchain: clang-cl, lld-link and llvm-ml with the MSVC CRT and Windows
# SDK fetched by xwin (tools/toolchain.ps1). Works from Windows and Linux hosts.
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl.cmake -DTESSERA_TOOLCHAIN=<.toolchain/host> -DTESSERA_ARCH=x64|x86
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES TESSERA_TOOLCHAIN TESSERA_ARCH)
if(NOT TESSERA_TOOLCHAIN OR NOT TESSERA_ARCH MATCHES "^(x64|x86)$")
  message(FATAL_ERROR "set TESSERA_TOOLCHAIN (.toolchain/<host>) and TESSERA_ARCH (x64 or x86)")
endif()

file(TO_CMAKE_PATH "${TESSERA_TOOLCHAIN}" TESSERA_TOOLCHAIN)  # backslashes would be escapes

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
if(TESSERA_ARCH STREQUAL "x64")
  set(CMAKE_SYSTEM_PROCESSOR AMD64)
  set(triple x86_64-pc-windows-msvc)
  set(xarch x86_64)
  set(masm -m64)
else()
  set(CMAKE_SYSTEM_PROCESSOR X86)
  set(triple i686-pc-windows-msvc)
  set(xarch x86)
  set(masm -m32)
endif()

set(exe "")
if(CMAKE_HOST_WIN32)
  set(exe ".exe")
endif()
set(llvm "${TESSERA_TOOLCHAIN}/llvm/bin")
set(CMAKE_C_COMPILER "${llvm}/clang-cl${exe}")
set(CMAKE_CXX_COMPILER "${llvm}/clang-cl${exe}")
set(CMAKE_ASM_MASM_COMPILER "${llvm}/llvm-ml${exe}")
set(CMAKE_LINKER "${llvm}/lld-link${exe}")
set(CMAKE_AR "${llvm}/llvm-lib${exe}")
set(CMAKE_RC_COMPILER "${llvm}/llvm-rc${exe}")
set(CMAKE_C_COMPILER_TARGET ${triple})
set(CMAKE_CXX_COMPILER_TARGET ${triple})

set(xwin "${TESSERA_TOOLCHAIN}/xwin")
set(inc "")
foreach(d crt/include sdk/include/ucrt sdk/include/um sdk/include/shared sdk/include/winrt)
  string(APPEND inc " /imsvc \"${xwin}/${d}\"")
endforeach()
set(CMAKE_C_FLAGS_INIT "${inc}")
set(CMAKE_CXX_FLAGS_INIT "${inc}")
set(CMAKE_ASM_MASM_FLAGS_INIT "${masm}")
set(libs "")
foreach(d crt/lib/${xarch} sdk/lib/um/${xarch} sdk/lib/ucrt/${xarch})
  string(APPEND libs " /libpath:\"${xwin}/${d}\"")
endforeach()
# No mt.exe here: /MANIFEST:NO keeps CMake from calling it, the later /MANIFEST:EMBED
# has lld-link embed the default manifest itself.
set(CMAKE_EXE_LINKER_FLAGS_INIT "${libs} /MANIFEST:NO /MANIFEST:EMBED")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${libs} /MANIFEST:NO /MANIFEST:EMBED")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${libs} /MANIFEST:NO /MANIFEST:EMBED")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
