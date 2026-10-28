# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Andy Taylor
cmake_minimum_required(VERSION 3.21)
project(UltraViolet VERSION 0.1.0 DESCRIPTION "Sun UltraSPARC (Ultra 5/10) emulator" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# The version as shown: the project version and its stage.
set(UV_VERSION "${PROJECT_VERSION}-alpha")

option(UV_BUILD_TESTS "Build unit tests" ON)
option(UV_WERROR "Treat warnings as errors" OFF)

add_library(uv_warnings INTERFACE)
if(MINGW)
  # The C99-conforming printf family (%zu, %lld), as the code expects.
  add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
endif()
if(MSVC)
  target_compile_options(uv_warnings INTERFACE /W4 /permissive- /Zc:__cplusplus)
  if(UV_WERROR)
    target_compile_options(uv_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(uv_warnings INTERFACE -Wall -Wextra -Wshadow -Wnon-virtual-dtor)
  if(UV_WERROR)
    target_compile_options(uv_warnings INTERFACE -Werror)
  endif()
endif()

add_library(uv_emu STATIC
  src/core/host_memory.cpp
  src/core/log.cpp
  src/core/pacer.cpp
  src/core/scheduler.cpp
  src/core/worker_pool.cpp
  src/jit/exec_buffer.cpp
  src/mem/bus.cpp
  src/cpu/ultrasparc/ultrasparc.cpp
  src/cpu/ultrasparc/mmu.cpp
  src/cpu/ultrasparc/fpu.cpp
  src/cpu/ultrasparc/disasm.cpp
  src/devices/pci.cpp
  src/devices/sabre.cpp
  src/devices/simba.cpp
  src/devices/rio_ebus.cpp
  src/devices/ns16550.cpp
  src/devices/sab82532.cpp
  src/devices/m48t59.cpp
  src/devices/ebus_misc.cpp
  src/devices/ebus_dma.cpp
  src/devices/fdc82077.cpp
  src/devices/ata.cpp
  src/devices/atapi_cdrom.cpp
  src/devices/cmd646.cpp
  src/storage/disk_image.cpp
  src/machine/ultra5_10/ultra5_10.cpp
  src/net/net_backend.cpp
  src/net/slirp_backend.cpp
  src/net/tap_backend.cpp
)
target_include_directories(uv_emu PUBLIC src)
target_link_libraries(uv_emu PRIVATE uv_warnings)
find_package(Threads REQUIRED)
target_link_libraries(uv_emu PUBLIC Threads::Threads)

# User-mode networking (optional)
option(UV_WITH_SLIRP "Build user-mode networking with libslirp" ON)
if(UV_WITH_SLIRP)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SLIRP IMPORTED_TARGET slirp)
  endif()
  if(SLIRP_FOUND)
    target_compile_definitions(uv_emu PRIVATE UV_HAVE_SLIRP=1)
    target_link_libraries(uv_emu PUBLIC PkgConfig::SLIRP)
    if(WIN32)
      target_link_libraries(uv_emu PUBLIC ws2_32)  # the network thread's own sockets
    endif()
  else()
    message(WARNING "libslirp not found: building without user-mode networking")
  endif()
endif()

add_executable(ultraviolet src/app/main.cpp)
target_link_libraries(ultraviolet PRIVATE uv_emu uv_warnings)
target_compile_definitions(ultraviolet PRIVATE UV_VERSION="${UV_VERSION}")

# The FPU relies on the host rounding mode and exception flags.
if(MSVC)
  set_source_files_properties(src/cpu/ultrasparc/fpu.cpp PROPERTIES COMPILE_OPTIONS "/fp:strict")
else()
  set_source_files_properties(src/cpu/ultrasparc/fpu.cpp PROPERTIES COMPILE_OPTIONS "-frounding-math;-ffp-contract=off")
endif()

if(UV_BUILD_TESTS)
  enable_testing()
  add_executable(uv_tests
    tests/test_main.cpp
    tests/core_tests.cpp
    tests/cpu_tests.cpp
    tests/fpu_tests.cpp
    tests/machine_tests.cpp
    tests/ide_tests.cpp
  )
  target_link_libraries(uv_tests PRIVATE uv_emu uv_warnings)
  add_test(NAME uv_tests COMMAND uv_tests)
endif()
