# ============================================================
# SSV6256P Userspace Monitor/Inject Driver - CMake Build
# C++14 Standard | libusb-1.0 | Cross-platform (macOS/Linux)
# ============================================================

cmake_minimum_required(VERSION 3.10)
project(ssv6xxx VERSION 2.0.0 LANGUAGES CXX)

# ── C++14 Standard ─────────────────────────────────────────
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SSV6XXX_BUILD_CLI "Build the sv6256p_monitor command-line application" ON)

# ── Compiler Warnings ──────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(-Wall -Wextra -Wpedantic -O2 -g)
endif()

# ── Find spdlog ───────────────────────────────────────────
find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
    find_path(SPDLOG_INCLUDE_DIR spdlog/spdlog.h
        HINTS /opt/homebrew/include /usr/local/include)
    if(SPDLOG_INCLUDE_DIR)
        add_library(spdlog::spdlog INTERFACE IMPORTED)
        set_target_properties(spdlog::spdlog PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${SPDLOG_INCLUDE_DIR}")
        set(spdlog_FOUND TRUE)
        message(STATUS "Found spdlog: ${SPDLOG_INCLUDE_DIR}")
    endif()
endif()

if(NOT spdlog_FOUND)
    message(FATAL_ERROR "spdlog not found. Install with: brew install spdlog (macOS) or apt install libspdlog-dev (Linux)")
endif()

# ── Find CLI11 ────────────────────────────────────────────
if(SSV6XXX_BUILD_CLI)
    find_package(CLI11 QUIET)
    if(NOT CLI11_FOUND)
        find_path(CLI11_INCLUDE_DIR CLI/CLI.hpp
            HINTS /opt/homebrew/include /usr/local/include)
        if(CLI11_INCLUDE_DIR)
            add_library(CLI11::CLI11 INTERFACE IMPORTED)
            set_target_properties(CLI11::CLI11 PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${CLI11_INCLUDE_DIR}")
            set(CLI11_FOUND TRUE)
            message(STATUS "Found CLI11: ${CLI11_INCLUDE_DIR}")
        endif()
    endif()

    if(NOT CLI11_FOUND)
        message(FATAL_ERROR "CLI11 not found. Install it or configure with -DSSV6XXX_BUILD_CLI=OFF")
    endif()
endif()

# ── Windows / MinGW (MSYS2) 依赖提示 ────────────────────────
if(WIN32 OR MINGW)
    list(APPEND CMAKE_PREFIX_PATH
        "E:/MSYS2/mingw64"
        "C:/msys64/mingw64"
        "C:/msys64/ucrt64")
    if(EXISTS "E:/MSYS2/mingw64/bin/pkg-config.exe")
        set(PKG_CONFIG_EXECUTABLE "E:/MSYS2/mingw64/bin/pkg-config.exe")
    elseif(EXISTS "C:/msys64/mingw64/bin/pkg-config.exe")
        set(PKG_CONFIG_EXECUTABLE "C:/msys64/mingw64/bin/pkg-config.exe")
    endif()
    if(DEFINED ENV{MINGW_PREFIX} AND IS_DIRECTORY "$ENV{MINGW_PREFIX}")
        list(APPEND CMAKE_PREFIX_PATH "$ENV{MINGW_PREFIX}")
    endif()
endif()

# ── Find libusb-1.0 ────────────────────────────────────────
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBUSB libusb-1.0)
endif()

if(NOT LIBUSB_FOUND)
    # Fallback: search Homebrew / system / MSYS2 paths
    find_path(LIBUSB_INCLUDE_DIRS libusb.h
        HINTS /opt/homebrew/include/libusb-1.0
              /usr/local/include/libusb-1.0
              /usr/include/libusb-1.0
              ${CMAKE_PREFIX_PATH}/include/libusb-1.0
              E:/MSYS2/mingw64/include/libusb-1.0
              C:/msys64/mingw64/include/libusb-1.0)
    find_library(LIBUSB_LIBRARIES usb-1.0
        HINTS /opt/homebrew/lib /usr/local/lib /usr/lib
              ${CMAKE_PREFIX_PATH}/lib
              E:/MSYS2/mingw64/lib
              C:/msys64/mingw64/lib)
    if(LIBUSB_INCLUDE_DIRS AND LIBUSB_LIBRARIES)
        set(LIBUSB_FOUND TRUE)
        message(STATUS "Found libusb-1.0: ${LIBUSB_LIBRARIES}")
    endif()
endif()

if(NOT LIBUSB_FOUND)
    message(FATAL_ERROR "libusb-1.0 not found. Install with: brew install libusb (macOS), apt install libusb-1.0-0-dev (Linux), or pacman -S mingw-w64-x86_64-libusb (MSYS2)")
endif()

# 源码使用 #include <libusb-1.0/libusb.h>，需要的是父目录。
set(SSV6XXX_LIBUSB_INCLUDE_DIRS ${LIBUSB_INCLUDE_DIRS})
if(LIBUSB_INCLUDE_DIRS MATCHES "libusb-1\\.0$")
    get_filename_component(_libusb_parent "${LIBUSB_INCLUDE_DIRS}" DIRECTORY)
    list(INSERT SSV6XXX_LIBUSB_INCLUDE_DIRS 0 "${_libusb_parent}")
endif()

# ── Source Files (C++14) ───────────────────────────────────
set(SOURCES
    src/ssv6xxx_embedded_firmware.cpp
    src/ssv6xxx_driver.cpp
    # 新的 C++14 驱动框架（逐步替换旧代码）
    src/ssv6xxx_usb_transport.cpp
    src/ssv6xxx_chip_init.cpp
    src/ssv6xxx_monitor_session.cpp
)

set(HEADERS
    src/ssv6xxx_embedded_firmware.hpp
    src/ssv6xxx_regs.hpp
    src/ssv6xxx_descriptors.hpp
    src/ssv6xxx_driver.hpp
    src/ssv6xxx_types.hpp
    src/ssv6xxx_exceptions.hpp
    src/ssv6xxx_usb_protocol.hpp
    src/ssv6xxx_usb_transport.hpp
    src/ssv6xxx_chip_init.hpp
    src/ssv6xxx_monitor_session.hpp
)

# ── Executable ─────────────────────────────────────────────
add_library(ssv6xxx ${SOURCES} ${HEADERS})
add_library(ssv6xxx::ssv6xxx ALIAS ssv6xxx)

target_include_directories(ssv6xxx PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    ${SSV6XXX_LIBUSB_INCLUDE_DIRS}
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/kernel-driver/L.SMAC.19Q3.2126.02_r3408/include)

if(LIBUSB_LIBRARY_DIRS)
    # Static-library consumers perform the final libusb link, so its search
    # directory must be part of the link interface as well.
    target_link_directories(ssv6xxx PUBLIC ${LIBUSB_LIBRARY_DIRS})
endif()

target_link_libraries(ssv6xxx PUBLIC
    spdlog::spdlog
    ${LIBUSB_LIBRARIES})

if(SSV6XXX_BUILD_CLI)
    add_executable(sv6256p_monitor src/main.cpp)
    target_link_libraries(sv6256p_monitor PRIVATE ssv6xxx::ssv6xxx CLI11::CLI11)

    add_executable(ssv_dual_link_test src/dual_link_test.cpp)
    target_link_libraries(ssv_dual_link_test PRIVATE ssv6xxx::ssv6xxx CLI11::CLI11)
    if(WIN32 OR MINGW)
        find_file(SSV6XXX_LIBUSB_DLL libusb-1.0.dll
            HINTS E:/MSYS2/mingw64/bin
                  C:/msys64/mingw64/bin
                  ${CMAKE_PREFIX_PATH}/bin)
        if(SSV6XXX_LIBUSB_DLL)
            add_custom_command(TARGET ssv_dual_link_test POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SSV6XXX_LIBUSB_DLL}"
                    "$<TARGET_FILE_DIR:ssv_dual_link_test>/"
                COMMENT "Copy libusb-1.0.dll next to ssv_dual_link_test"
                VERBATIM)
        endif()
    endif()
endif()

# ── Platform-specific ──────────────────────────────────────
if(APPLE)
    target_link_libraries(ssv6xxx PRIVATE
        "-framework CoreFoundation"
        "-framework IOKit")
endif()

# ── Build Info ─────────────────────────────────────────────
message(STATUS "=== SV6256P Monitor Build Config ===")
message(STATUS "  C++ Standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler:     ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  libusb:       ${LIBUSB_INCLUDE_DIRS}")
message(STATUS "  Library:      ssv6xxx")
message(STATUS "  Application:  sv6256p_monitor")
