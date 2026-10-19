cmake_minimum_required(VERSION 3.25)
project(unity-clipboard-indicator VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(GNUInstallDirs)
include(CheckIPOSupported)
find_package(PkgConfig REQUIRED)

pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
pkg_check_modules(PIXBUF REQUIRED IMPORTED_TARGET gdk-pixbuf-2.0)
pkg_check_modules(XLIBS REQUIRED IMPORTED_TARGET x11 xfixes xtst)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

# Small, lean binaries: no exceptions/RTTI (GLib reports errors via GError),
# dead-code elimination and LTO.
add_compile_options(-Wall -Wextra -Wno-missing-field-initializers -fno-exceptions -fno-rtti
                    -ffunction-sections -fdata-sections -fvisibility=hidden)
add_link_options(-Wl,--gc-sections -Wl,--as-needed -Wl,-O1)
check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg)
if(ipo_ok)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

set(CI_DATADIR "${CMAKE_INSTALL_FULL_DATADIR}/clipboard-indicator")
add_compile_definitions(CI_VERSION="${PROJECT_VERSION}" CI_DATADIR="${CI_DATADIR}")

add_library(ci_common STATIC src/common/settings.cpp)
target_include_directories(ci_common PUBLIC src)
target_link_libraries(ci_common PUBLIC PkgConfig::GIO)

add_executable(clipboard-indicatord
  src/daemon/main.cpp
  src/daemon/app.cpp
  src/daemon/history.cpp
  src/daemon/indicator.cpp
  src/daemon/x11.cpp)
target_include_directories(clipboard-indicatord PRIVATE src/daemon)
target_link_libraries(clipboard-indicatord PRIVATE ci_common PkgConfig::PIXBUF PkgConfig::XLIBS)

add_executable(clipboard-indicator-ui
  src/ui/main.cpp
  src/ui/popup.cpp
  src/ui/settings_window.cpp
  src/ui/style.cpp)
target_include_directories(clipboard-indicator-ui PRIVATE src/ui)
target_link_libraries(clipboard-indicator-ui PRIVATE ci_common PkgConfig::GTK3)

# The daemon looks for icons next to itself first, so the build tree runs
# without installing.
file(COPY data/icons DESTINATION ${CMAKE_BINARY_DIR})

install(TARGETS clipboard-indicatord clipboard-indicator-ui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY data/icons DESTINATION ${CMAKE_INSTALL_DATADIR}/clipboard-indicator)
install(FILES data/icons/ci-clipboard-panel.svg data/icons/ci-clipboard-panel-private.svg
              data/icons/ci-clipboard-panel-blink.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/status)
configure_file(data/clipboard-indicator-autostart.desktop.in clipboard-indicator-autostart.desktop @ONLY)
configure_file(data/clipboard-indicator-settings.desktop.in clipboard-indicator-settings.desktop @ONLY)
# Autostart: by default into ~/.config/autostart of the user running the
# install (the invoking user under sudo). Packagers (DESTDIR set) or
# -DCI_AUTOSTART_SYSTEM=ON get the system-wide xdg autostart directory.
option(CI_AUTOSTART_SYSTEM "Install the autostart entry into <sysconfdir>/xdg/autostart" OFF)
if(CI_AUTOSTART_SYSTEM)
  install(FILES ${CMAKE_BINARY_DIR}/clipboard-indicator-autostart.desktop
          DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/xdg/autostart RENAME clipboard-indicator.desktop)
else()
  set(CI_AUTOSTART_SRC "${CMAKE_BINARY_DIR}/clipboard-indicator-autostart.desktop")
  set(code [[
    set(src "@CI_AUTOSTART_SRC@")
    set(user "")
    set(home "")
    if(DEFINED ENV{DESTDIR} AND NOT "$ENV{DESTDIR}" STREQUAL "")
      # Packaging: file(INSTALL) prepends DESTDIR itself.
      set(sysconf "@CMAKE_INSTALL_SYSCONFDIR@")
      if(NOT IS_ABSOLUTE "${sysconf}")
        set(sysconf "${CMAKE_INSTALL_PREFIX}/${sysconf}")
      endif()
      file(INSTALL "${src}" DESTINATION "${sysconf}/xdg/autostart" RENAME clipboard-indicator.desktop)
    else()
      set(user "$ENV{SUDO_USER}")
      if(user STREQUAL "" OR user STREQUAL "root")
        set(user "")
        set(home "$ENV{HOME}")
      else()
        execute_process(COMMAND getent passwd "${user}" OUTPUT_VARIABLE pw OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REPLACE ":" ";" pw "${pw}")
        list(LENGTH pw n)
        if(n GREATER 5)
          list(GET pw 5 home)
        endif()
      endif()
      if(home STREQUAL "")
        message(WARNING "Cannot determine the home directory; autostart entry not installed")
      endif()
    endif()
    if(NOT home STREQUAL "")
      set(created "")
      foreach(d "${home}/.config" "${home}/.config/autostart")
        if(NOT EXISTS "${d}")
          file(MAKE_DIRECTORY "${d}")
          list(APPEND created "${d}")
        endif()
      endforeach()
      set(dest "${home}/.config/autostart/clipboard-indicator.desktop")
      message(STATUS "Installing: ${dest}")
      file(COPY_FILE "${src}" "${dest}")
      if(NOT user STREQUAL "")
        # Installed via sudo: hand the file (and any new dirs) to the user.
        execute_process(COMMAND id -gn "${user}" OUTPUT_VARIABLE group OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND chown "${user}:${group}" ${created} "${dest}")
      endif()
    endif()
  ]])
  string(CONFIGURE "${code}" code @ONLY)
  install(CODE "${code}")
endif()
install(FILES ${CMAKE_BINARY_DIR}/clipboard-indicator-settings.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications RENAME clipboard-indicator-settings.desktop)
