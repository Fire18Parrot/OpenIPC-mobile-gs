# SPDX-License-Identifier: GPL-3.0-only

function(openipc_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE openipc_gs_core)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

openipc_add_test(test_alink)
openipc_add_test(test_rtp)
openipc_add_test(test_msp)
openipc_add_test(test_mavlink)

# The interop test compiles wfb-ng's transmitter so packets are produced by
# upstream's encoder rather than by our own understanding of the format.
if(OPENIPC_WITH_WFB)
    add_executable(test_wfb_interop test_wfb_interop.cpp ${OPENIPC_THIRD_PARTY}/wfb-ng/src/tx.cpp)
    target_link_libraries(test_wfb_interop PRIVATE openipc_gs_core wfb_ng)
    target_include_directories(test_wfb_interop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # rx.cpp guards its main() behind __WFB_RX_SHARED_LIBRARY__, but tx.cpp has
    # no such switch upstream, so its main() would collide with the test's.
    # Renaming the symbol is the least invasive way to reuse the file without
    # patching the submodule - scoped to tx.cpp alone, or it would rename the
    # test's own main() too. WFB_VERSION is normally injected by wfb-ng's
    # Makefile.
    set_source_files_properties(${OPENIPC_THIRD_PARTY}/wfb-ng/src/tx.cpp
        PROPERTIES COMPILE_DEFINITIONS "main=wfb_tx_main_unused;WFB_VERSION=\"openipc-mobile-gs-test\"")
    add_test(NAME test_wfb_interop COMMAND test_wfb_interop)

    # The RubyFPV spike. Note what this is NOT: upstream's licence carries a
    # field-of-use restriction GPL-3 cannot absorb, so unlike the interop test
    # above there is no upstream transmitter here to check ourselves against -
    # both sides of this test are ours. It proves the reassembler is sound, not
    # that the format was read correctly. A real capture is what settles that.
    add_executable(test_ruby test_ruby.cpp)
    target_link_libraries(test_ruby PRIVATE openipc_gs_core wfb_ng)
    target_include_directories(test_ruby PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME test_ruby COMMAND test_ruby)
endif()
