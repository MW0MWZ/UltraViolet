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
  src/cpu/ultrasparc/jit_a64.cpp
  src/cpu/ultrasparc/jit_x64.cpp
  src/devices/pci.cpp
  src/devices/sabre.cpp
  src/devices/simba.cpp
  src/devices/rio_ebus.cpp
  src/devices/ns16550.cpp
  src/devices/sab82532.cpp
  src/devices/serial.cpp
  src/devices/sun_keyboard.cpp
  src/devices/m48t59.cpp
  src/devices/ebus_misc.cpp
  src/devices/ebus_dma.cpp
  src/devices/fdc82077.cpp
  src/devices/hme.cpp
  src/devices/mach64.cpp
  src/devices/mach64_engine.cpp
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

# The front end's parts that need no window: settings, PROM images, the
# machine's session, the terminal emulator, leaving the terminal.
add_library(uv_app STATIC
  src/app/detach.cpp
  src/app/proms.cpp
  src/app/session.cpp
  src/app/settings.cpp
  src/app/terminal.cpp
)
target_include_directories(uv_app PUBLIC src)
target_link_libraries(uv_app PUBLIC uv_emu PRIVATE uv_warnings)
if(WIN32)
  target_link_libraries(uv_app PRIVATE shell32 ole32)  # the Documents folder
endif()

add_executable(ultraviolet src/app/main.cpp)
target_link_libraries(ultraviolet PRIVATE uv_app uv_warnings)
target_compile_definitions(ultraviolet PRIVATE UV_VERSION="${UV_VERSION}")

option(UV_WITH_SDL3 "Build the SDL3 display window" ON)
if(UV_WITH_SDL3)
  find_package(SDL3 CONFIG QUIET)
  if(SDL3_FOUND)
    # Dear ImGui, for the window's menus
    add_library(uv_imgui STATIC
      third_party/imgui/imgui.cpp
      third_party/imgui/imgui_draw.cpp
      third_party/imgui/imgui_tables.cpp
      third_party/imgui/imgui_widgets.cpp
      third_party/imgui/misc/cpp/imgui_stdlib.cpp
      third_party/imgui/backends/imgui_impl_sdl3.cpp
      third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp
    )
    target_include_directories(uv_imgui PUBLIC third_party/imgui)
    target_compile_definitions(uv_imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    target_link_libraries(uv_imgui PUBLIC SDL3::SDL3)

    target_sources(ultraviolet PRIVATE
      src/app/gui.cpp
      src/app/terminal_window.cpp
    )
    target_compile_definitions(ultraviolet PRIVATE UV_HAVE_SDL3=1)
    target_link_libraries(ultraviolet PRIVATE uv_imgui SDL3::SDL3)

    if(APPLE)
      # A macOS application: UltraViolet.app, with the system menu bar (and
      # its icon, made from graphics/UltraViolet_icon_1024.png with sips and
      # iconutil when that artwork is there). build/ultraviolet links to the
      # program inside, for the terminal. `cmake --install build --prefix
      # DIR` makes a self-contained copy in DIR, with the libraries it uses
      # (SDL3, libslirp, ...) inside it.
      enable_language(OBJCXX)
      target_sources(ultraviolet PRIVATE src/app/menu_mac.mm)
      set_source_files_properties(src/app/menu_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
      target_link_libraries(ultraviolet PRIVATE "-framework Cocoa")

      set(UV_ICON_PNG ${CMAKE_CURRENT_SOURCE_DIR}/graphics/UltraViolet_icon_1024.png)
      set(UV_ICON_FILE "")
      if(EXISTS ${UV_ICON_PNG})
        set(UV_ICONSET ${CMAKE_CURRENT_BINARY_DIR}/UltraViolet.iconset)
        set(UV_ICNS ${CMAKE_CURRENT_BINARY_DIR}/UltraViolet.icns)
        set(UV_ICON_COMMANDS)
        foreach(size 16 32 128 256 512)
          math(EXPR double "${size} * 2")
          list(APPEND UV_ICON_COMMANDS
            COMMAND sips -z ${size} ${size} ${UV_ICON_PNG} --out ${UV_ICONSET}/icon_${size}x${size}.png
            COMMAND sips -z ${double} ${double} ${UV_ICON_PNG} --out ${UV_ICONSET}/icon_${size}x${size}@2x.png)
        endforeach()
        add_custom_command(OUTPUT ${UV_ICNS}
          COMMAND ${CMAKE_COMMAND} -E make_directory ${UV_ICONSET}
          ${UV_ICON_COMMANDS}
          COMMAND iconutil -c icns ${UV_ICONSET} -o ${UV_ICNS}
          DEPENDS ${UV_ICON_PNG}
          COMMENT "Making the application icon"
          VERBATIM)
        target_sources(ultraviolet PRIVATE ${UV_ICNS})
        set_source_files_properties(${UV_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        set(UV_ICON_FILE "UltraViolet")
      endif()

      configure_file(cmake/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
      set_target_properties(ultraviolet PROPERTIES
        MACOSX_BUNDLE TRUE
        OUTPUT_NAME "UltraViolet"
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist)
      add_custom_command(TARGET ultraviolet POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_CURRENT_BINARY_DIR}/ultraviolet
        COMMAND ${CMAKE_COMMAND} -E create_symlink "UltraViolet.app/Contents/MacOS/UltraViolet"
                ${CMAKE_CURRENT_BINARY_DIR}/ultraviolet
        VERBATIM)

      install(TARGETS ultraviolet BUNDLE DESTINATION .)
      # The Info.plist is installed explicitly as well, and the libraries are
      # gathered from the program's own path, so neither depends on the other.
      install(FILES ${CMAKE_CURRENT_BINARY_DIR}/Info.plist DESTINATION "UltraViolet.app/Contents")
      install(CODE [[
        include(BundleUtilities)
        set(app "${CMAKE_INSTALL_PREFIX}/UltraViolet.app")
        fixup_bundle("${app}/Contents/MacOS/UltraViolet" "" "")
        # Signed for this machine (ad hoc): copying the libraries in changed them.
        execute_process(COMMAND codesign --force --deep --sign - "${app}" COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND codesign --verify --deep --strict "${app}" COMMAND_ERROR_IS_FATAL ANY)
      ]])
    endif()
  else()
    message(WARNING "SDL3 not found: building without a display window")
  endif()
endif()

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
    tests/hme_tests.cpp
    tests/input_tests.cpp
    tests/mach64_tests.cpp
    tests/jit_tests.cpp
  )
  target_link_libraries(uv_tests PRIVATE uv_emu uv_warnings)
  add_test(NAME uv_tests COMMAND uv_tests)
endif()
