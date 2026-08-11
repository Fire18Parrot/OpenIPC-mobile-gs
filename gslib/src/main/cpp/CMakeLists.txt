# SPDX-License-Identifier: GPL-3.0-only
#
# The Android native build: devourer + wfb-ng + the portable engine under
# core/, behind one JNI library.
#
# Everything is built from the submodules in third_party/, so there are no
# prebuilt binaries in the tree and the whole radio stack is reproducible from
# source. Run `git submodule update --init --recursive` before building.

cmake_minimum_required(VERSION 3.22.1)
project(openipc_mobile_gs CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(THIRD_PARTY "${REPO_ROOT}/third_party")

foreach(module libusb libsodium wfb-ng devourer)
    if(NOT EXISTS "${THIRD_PARTY}/${module}/.git" AND NOT EXISTS "${THIRD_PARTY}/${module}/README.md")
        message(FATAL_ERROR
            "Submodule ${module} is missing. Run: git submodule update --init --recursive")
    endif()
endforeach()

# --- libusb ------------------------------------------------------------------
#
# Source list taken from libusb's own android/jni/libusb.mk so it stays in step
# with upstream. On Android we never enumerate devices ourselves - the app is
# handed a file descriptor by the system after the user grants USB permission -
# but linux_usbfs is still the backend that drives it.

# core.c includes version_describe.h, which libusb's autotools build generates
# on the fly via build-aux/gen-describe.sh. It only ever defines one string, so
# writing it here is simpler than shelling out to that script from the NDK
# build - and keeps the result reproducible rather than varying with the git
# state of the submodule.
set(LIBUSB_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/libusb_generated")
file(MAKE_DIRECTORY "${LIBUSB_GENERATED}")
file(WRITE "${LIBUSB_GENERATED}/version_describe.h"
     "#define LIBUSB_DESCRIBE \"OpenIPC-mobile-gs vendored build\"\n")

add_library(usb-1.0 STATIC
    ${THIRD_PARTY}/libusb/libusb/core.c
    ${THIRD_PARTY}/libusb/libusb/descriptor.c
    ${THIRD_PARTY}/libusb/libusb/hotplug.c
    ${THIRD_PARTY}/libusb/libusb/io.c
    ${THIRD_PARTY}/libusb/libusb/sync.c
    ${THIRD_PARTY}/libusb/libusb/strerror.c
    ${THIRD_PARTY}/libusb/libusb/os/linux_usbfs.c
    ${THIRD_PARTY}/libusb/libusb/os/events_posix.c
    ${THIRD_PARTY}/libusb/libusb/os/threads_posix.c
    ${THIRD_PARTY}/libusb/libusb/os/linux_netlink.c
)
target_include_directories(usb-1.0 PUBLIC ${THIRD_PARTY}/libusb/libusb)
target_include_directories(usb-1.0 PRIVATE
    ${THIRD_PARTY}/libusb/libusb/os
    ${THIRD_PARTY}/libusb/android
    ${LIBUSB_GENERATED}
)
target_compile_options(usb-1.0 PRIVATE -fvisibility=hidden)
target_link_libraries(usb-1.0 PRIVATE log)

# --- libsodium ---------------------------------------------------------------
#
# libsodium ships an autotools build; rather than run it from Gradle we compile
# the portable sources directly. The optimised backends guard themselves behind
# HAVE_* macros that autoconf would define, so without a config.h they compile
# to nothing and the portable implementations are used - correct, and the crypto
# here is nowhere near the bottleneck.
file(GLOB_RECURSE SODIUM_SOURCES ${THIRD_PARTY}/libsodium/src/libsodium/*.c)

# version.h is normally produced by configure.
file(STRINGS ${THIRD_PARTY}/libsodium/configure.ac SODIUM_VERSION_LINE
     REGEX "^AC_INIT\\(" LIMIT_COUNT 1)
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" SODIUM_VERSION "${SODIUM_VERSION_LINE}")
if(NOT SODIUM_VERSION)
    set(SODIUM_VERSION "1.0.20")
endif()
set(SODIUM_LIBRARY_VERSION_MAJOR 26)
set(SODIUM_LIBRARY_VERSION_MINOR 2)
set(SODIUM_LIBRARY_MINIMAL_DEF "")
set(VERSION ${SODIUM_VERSION})
configure_file(
    ${THIRD_PARTY}/libsodium/src/libsodium/include/sodium/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/sodium_generated/sodium/version.h
    @ONLY)

add_library(sodium STATIC ${SODIUM_SOURCES})
target_include_directories(sodium PUBLIC
    ${THIRD_PARTY}/libsodium/src/libsodium/include
    ${CMAKE_CURRENT_BINARY_DIR}/sodium_generated
)
target_include_directories(sodium PRIVATE
    ${THIRD_PARTY}/libsodium/src/libsodium/include/sodium
    # sodium/version.c includes "version.h" unqualified, so the generated
    # header's own directory has to be on the path as well as its parent
    # (which serves the <sodium/version.h> form used elsewhere).
    ${CMAKE_CURRENT_BINARY_DIR}/sodium_generated/sodium
)
target_compile_definitions(sodium PUBLIC SODIUM_STATIC=1)
target_compile_definitions(sodium PRIVATE
    CONFIGURED=1
    _GNU_SOURCE=1
    DEV_MODE=0
)

# --- wfb-ng ------------------------------------------------------------------
#
# rx.cpp includes pcap.h for a Receiver class we never instantiate - devourer
# feeds the aggregator instead. Both live in one translation unit, so the pcap
# symbols still have to resolve; a stub that fails loudly is far lighter than
# putting libpcap on the phone. See compat/pcap.h.
add_library(pcap_stub STATIC compat/pcap_stub.c)
target_include_directories(pcap_stub PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/compat)

# rx.cpp's main() is dropped by __WFB_RX_SHARED_LIBRARY__; PREINCLUDE_FILE
# redirects its logging into logcat.
add_library(wfb_ng STATIC
    ${THIRD_PARTY}/wfb-ng/src/zfex.c
    ${THIRD_PARTY}/wfb-ng/src/radiotap.c
    ${THIRD_PARTY}/wfb-ng/src/rx.cpp
    ${THIRD_PARTY}/wfb-ng/src/wifibroadcast.cpp
)
target_include_directories(wfb_ng PUBLIC ${THIRD_PARTY}/wfb-ng/src ${THIRD_PARTY}/wfb-ng)
target_include_directories(wfb_ng PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat)
target_compile_definitions(wfb_ng PUBLIC __WFB_RX_SHARED_LIBRARY__)
target_compile_definitions(wfb_ng PRIVATE
    PREINCLUDE_FILE=<${CMAKE_CURRENT_SOURCE_DIR}/wfb_log.h>
    ZFEX_UNROLL_ADDMUL_SIMD=8
    ZFEX_INLINE_ADDMUL
    ZFEX_INLINE_ADDMUL_SIMD
    ZFEX_USE_ARM_NEON
)
target_link_libraries(wfb_ng PUBLIC sodium pcap_stub log)

# --- devourer ----------------------------------------------------------------
#
# devourer's own CMakeLists resolves libusb through pkg-config, which the NDK
# toolchain does not provide, so the source groups are mirrored here. Chips
# compiled in: Jaguar1 (RTL8812AU/8811AU/8821AU) and Jaguar3 8822E
# (RTL8812EU/8822EU) - between them the adapters OpenIPC users actually fly.
add_library(devourer STATIC
    ${THIRD_PARTY}/devourer/src/HalmacCfgParam.cpp
    ${THIRD_PARTY}/devourer/src/ParsedRadioPacket.cpp
    ${THIRD_PARTY}/devourer/src/Radiotap.c
    ${THIRD_PARTY}/devourer/src/RadiotapBuilder.cpp
    ${THIRD_PARTY}/devourer/src/RtlAdapter.cpp
    ${THIRD_PARTY}/devourer/src/UsbTransport.cpp
    ${THIRD_PARTY}/devourer/src/UsbDeviceLock.cpp
    ${THIRD_PARTY}/devourer/src/UsbOpen.cpp
    ${THIRD_PARTY}/devourer/src/TxMode.cpp
    ${THIRD_PARTY}/devourer/src/TxPower.cpp
    ${THIRD_PARTY}/devourer/src/LinkHealth.cpp
    ${THIRD_PARTY}/devourer/src/LaCapture.cpp
    ${THIRD_PARTY}/devourer/src/SignalStop.cpp
    ${THIRD_PARTY}/devourer/src/WiFiDriver.cpp
    ${THIRD_PARTY}/devourer/src/PhyTableLoader.cpp
    ${THIRD_PARTY}/devourer/src/chanmig/ChannelScore.cpp
    ${THIRD_PARTY}/devourer/src/chanmig/MigGate.cpp

    # Jaguar1: RTL8812AU / 8811AU / 8821AU
    ${THIRD_PARTY}/devourer/hal/hal8812a_fw.c
    ${THIRD_PARTY}/devourer/hal/hal8821a_fw.c
    ${THIRD_PARTY}/devourer/hal/Hal8812PwrSeq.c
    ${THIRD_PARTY}/devourer/hal/Hal8821APwrSeq.c
    ${THIRD_PARTY}/devourer/hal/Hal8812a_TxPwrTrack.cpp
    ${THIRD_PARTY}/devourer/src/jaguar1/RtlJaguarDevice.cpp
    ${THIRD_PARTY}/devourer/src/jaguar1/HalModule.cpp
    ${THIRD_PARTY}/devourer/src/jaguar1/RadioManagementModule.cpp
    ${THIRD_PARTY}/devourer/src/jaguar1/EepromManager.cpp
    ${THIRD_PARTY}/devourer/src/jaguar1/FirmwareManager.cpp
    ${THIRD_PARTY}/devourer/src/jaguar1/Iqk8812a.cpp
    ${THIRD_PARTY}/devourer/src/jaguar1/PhydmWatchdog.cpp
    ${THIRD_PARTY}/devourer/src/jaguar1/PowerTracking8812a.cpp
    ${THIRD_PARTY}/devourer/src/jaguar1/FrameParser.cpp
    ${THIRD_PARTY}/devourer/src/jaguar1/BbDbgportReader.cpp

    # Jaguar3 shared dispatchers
    ${THIRD_PARTY}/devourer/src/jaguar3/RtlJaguar3Device.cpp
    ${THIRD_PARTY}/devourer/src/jaguar3/HalJaguar3.cpp
    ${THIRD_PARTY}/devourer/src/jaguar3/HalmacJaguar3Fw.cpp
    ${THIRD_PARTY}/devourer/src/jaguar3/HalmacJaguar3MacInit.cpp
    ${THIRD_PARTY}/devourer/src/jaguar3/RadioManagementJaguar3.cpp
    ${THIRD_PARTY}/devourer/src/jaguar3/PhydmRuntimeJaguar3.cpp
    ${THIRD_PARTY}/devourer/src/jaguar3/PhyTableLoaderJaguar3.cpp
    ${THIRD_PARTY}/devourer/src/jaguar3/Phy8822cTables.cpp
    ${THIRD_PARTY}/devourer/src/jaguar3/Halrf8822c.cpp

    # Jaguar3 8822E: RTL8812EU / RTL8822EU
    ${THIRD_PARTY}/devourer/hal/hal8822e_fw.c
    ${THIRD_PARTY}/devourer/hal/phydm/rtl8822e/Hal8822e_PhyTables.c
    ${THIRD_PARTY}/devourer/src/jaguar3/Phy8822eTables.cpp
    ${THIRD_PARTY}/devourer/src/jaguar3/Halrf8822e.cpp
)
target_include_directories(devourer PUBLIC
    ${THIRD_PARTY}/devourer
    ${THIRD_PARTY}/devourer/src
    ${THIRD_PARTY}/devourer/hal
    ${THIRD_PARTY}/devourer/hal/phydm/rtl8822c
    ${THIRD_PARTY}/devourer/hal/phydm/rtl8822e
)
target_compile_definitions(devourer PUBLIC
    DEVOURER_HAVE_JAGUAR1=1
    DEVOURER_HAVE_JAGUAR3=1
    DEVOURER_HAVE_JAGUAR3_8822E=1
)
target_link_libraries(devourer PUBLIC usb-1.0 log)
set_property(TARGET devourer PROPERTY CXX_STANDARD 20)

# --- the portable engine -----------------------------------------------------
add_library(openipc_gs_core STATIC
    ${REPO_ROOT}/core/src/alink/alink.cpp
    ${REPO_ROOT}/core/src/msp/displayport.cpp
    ${REPO_ROOT}/core/src/mavlink/router.cpp
    ${REPO_ROOT}/core/src/rtp/depacketizer.cpp
    ${REPO_ROOT}/core/src/wfb/link_stats.cpp
    ${REPO_ROOT}/core/src/wfb/wfb_receiver.cpp
    ${REPO_ROOT}/core/src/wfb/session_keys.cpp
)
target_include_directories(openipc_gs_core PUBLIC ${REPO_ROOT}/core/include)
target_link_libraries(openipc_gs_core PUBLIC wfb_ng sodium)
target_compile_definitions(openipc_gs_core PUBLIC OPENIPC_HAVE_WFB=1)

# --- the JNI bridge ----------------------------------------------------------
add_library(openipc_gs SHARED
    jni_bridge.cpp
    ground_station.cpp
    devourer_source.cpp
    udp_source.cpp
)
target_include_directories(openipc_gs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(openipc_gs
    openipc_gs_core
    devourer
    usb-1.0
    android
    log
)
