# SPDX-License-Identifier: GPL-3.0-only
#
# The portable ground-station engine.
#
# This library deliberately knows nothing about Android: it builds for the host
# too, which is what lets the protocol code be unit tested off-device. The
# Android module adds a JNI layer on top; see gslib/src/main/cpp.

cmake_minimum_required(VERSION 3.22)
project(openipc_gs_core CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

option(OPENIPC_TESTS "Build host unit tests" OFF)
option(OPENIPC_WITH_WFB "Build the wfb-ng receive path (needs libsodium)" ON)
option(OPENIPC_WITH_DEVOURER "Build the devourer USB driver backend (needs libusb)" OFF)

set(OPENIPC_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/../third_party" CACHE PATH
    "Location of the wfb-ng and devourer submodules")

add_library(openipc_gs_core STATIC
    src/alink/alink.cpp
    src/msp/displayport.cpp
    src/mavlink/router.cpp
    src/rtp/depacketizer.cpp
    src/wfb/link_stats.cpp
)

target_include_directories(openipc_gs_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

find_package(Threads REQUIRED)
target_link_libraries(openipc_gs_core PUBLIC Threads::Threads)

# --- wfb-ng receive path -----------------------------------------------------
#
# wfb-ng's Aggregator is compiled straight from the submodule rather than
# reimplemented, so the crypto, the FEC and the block reassembly are upstream's
# and stay bit-compatible with any air unit. __WFB_RX_SHARED_LIBRARY__ drops
# rx.cpp's main(); PREINCLUDE_FILE redirects its logging at our discretion.
if(OPENIPC_WITH_WFB)
    set(WFB_SRC "${OPENIPC_THIRD_PARTY}/wfb-ng/src")
    if(NOT EXISTS "${WFB_SRC}/rx.cpp")
        message(FATAL_ERROR
            "wfb-ng submodule missing at ${WFB_SRC}. Run: git submodule update --init --recursive")
    endif()

    add_library(wfb_ng STATIC
        ${WFB_SRC}/zfex.c
        ${WFB_SRC}/radiotap.c
        ${WFB_SRC}/rx.cpp
        ${WFB_SRC}/wifibroadcast.cpp
    )
    target_include_directories(wfb_ng PUBLIC ${WFB_SRC} ${OPENIPC_THIRD_PARTY}/wfb-ng)
    target_compile_definitions(wfb_ng PUBLIC __WFB_RX_SHARED_LIBRARY__)
    # zfex's SIMD paths: NEON on the phone, SSSE3 on the CI host.
    target_compile_definitions(wfb_ng PRIVATE
        ZFEX_UNROLL_ADDMUL_SIMD=8
        ZFEX_INLINE_ADDMUL
        ZFEX_INLINE_ADDMUL_SIMD
    )
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm")
        target_compile_definitions(wfb_ng PRIVATE ZFEX_USE_ARM_NEON)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i.86")
        target_compile_definitions(wfb_ng PRIVATE ZFEX_USE_INTEL_SSSE3)
    endif()

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
    # rx.cpp includes pcap.h for its own pcap-based Receiver, which we do not
    # use - devourer feeds the aggregator instead - but the header must resolve.
    pkg_check_modules(PCAP IMPORTED_TARGET libpcap)
    target_link_libraries(wfb_ng PUBLIC PkgConfig::SODIUM)
    if(TARGET PkgConfig::PCAP)
        target_link_libraries(wfb_ng PUBLIC PkgConfig::PCAP)
    else()
        target_link_libraries(wfb_ng PUBLIC pcap)
    endif()

    target_sources(openipc_gs_core PRIVATE
        src/wfb/wfb_receiver.cpp
        src/wfb/session_keys.cpp
        # The RubyFPV spike. It rides on the wfb-ng target only for zfex:
        # Ruby's FEC is the same Rizzo GF(2^8) code, so the one part of that
        # protocol that would have been painful to reimplement is already here
        # under a licence we can actually use. Nothing else is shared.
        src/ruby/ruby_receiver.cpp
    )
    target_link_libraries(openipc_gs_core PUBLIC wfb_ng)
    target_compile_definitions(openipc_gs_core PUBLIC OPENIPC_HAVE_WFB=1)
endif()

if(OPENIPC_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
