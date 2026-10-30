# adifedit - a GTK4 viewer and editor for ham radio ADIF logs.
# Copyright (C) 2026 Nicholas Heath
# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(test_document test_document.c)
target_link_libraries(test_document PRIVATE adifcore)
target_compile_options(test_document PRIVATE ${ADIFEDIT_C_FLAGS})
add_test(NAME document COMMAND test_document)

add_executable(test_batch_stamp test_batch_stamp.c)
target_link_libraries(test_batch_stamp PRIVATE adifcore)
target_compile_options(test_batch_stamp PRIVATE ${ADIFEDIT_C_FLAGS})
target_compile_definitions(test_batch_stamp PRIVATE
    EXAMPLE_BST="${CMAKE_SOURCE_DIR}/assets/stamps/example.bst"
    TEST_BUILDDIR="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME batch_stamp COMMAND test_batch_stamp)

add_executable(test_io test_io.c)
target_link_libraries(test_io PRIVATE adifcore)
target_compile_options(test_io PRIVATE ${ADIFEDIT_C_FLAGS})
add_test(NAME io COMMAND test_io)

add_executable(test_known_fields test_known_fields.c)
target_link_libraries(test_known_fields PRIVATE adifcore)
target_compile_options(test_known_fields PRIVATE ${ADIFEDIT_C_FLAGS})
add_test(NAME known_fields COMMAND test_known_fields)

set_tests_properties(document batch_stamp io known_fields PROPERTIES
    ENVIRONMENT "G_DEBUG=fatal-warnings;G_TEST_SRCDIR=${CMAKE_CURRENT_SOURCE_DIR}")

if(NOT WIN32)
    # The installed desktop entry and AppStream metadata must validate.
    # Registered only when the validators are installed (appstream,
    # desktop-file-utils); --no-net keeps it offline.
    find_program(APPSTREAMCLI appstreamcli)
    find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
    if(APPSTREAMCLI)
        add_test(NAME metainfo COMMAND ${APPSTREAMCLI} validate --no-net --pedantic
            ${CMAKE_SOURCE_DIR}/io.github.nheathlabs.adifedit.metainfo.xml)
    else()
        message(STATUS "appstreamcli not found: the metainfo test is not registered")
    endif()
    if(DESKTOP_FILE_VALIDATE)
        add_test(NAME desktop_file COMMAND ${DESKTOP_FILE_VALIDATE}
            ${CMAKE_SOURCE_DIR}/io.github.nheathlabs.adifedit.desktop)
    else()
        message(STATUS "desktop-file-validate not found: the desktop_file test is not registered")
    endif()

    # The one view test opens a real window, so tests/run_headless.sh runs it
    # in its own headless cage session; without cage it reports "skipped".
    # Not built for Windows, which has no cage.
    add_executable(test_table_scroll test_table_scroll.c)
    target_link_libraries(test_table_scroll PRIVATE adifeditui)
    target_compile_options(test_table_scroll PRIVATE ${ADIFEDIT_C_FLAGS})
    add_test(NAME table_scroll
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_headless.sh $<TARGET_FILE:test_table_scroll>)
    # Under the asan preset, leak checking is off for this test only:
    # GTK, Pango and fontconfig keep caches (and GTK some column objects)
    # alive past exit, which LeakSanitizer can't tell from a real leak.
    # Its memory errors are still caught; the headless tests keep leak
    # checking. Harmless in a build without ASan.
    set_tests_properties(table_scroll PROPERTIES SKIP_RETURN_CODE 77
        ENVIRONMENT "G_DEBUG=fatal-warnings;GSK_RENDERER=cairo;ASAN_OPTIONS=abort_on_error=1:detect_leaks=0")
endif()

# The parser fuzz target. With ADIFEDIT_FUZZ (the clang `fuzz` preset)
# libFuzzer drives it; run it by hand (CONTRIBUTING.md "Fuzzing").
# Otherwise it has its own main(), and ctest replays the saved inputs in
# fuzz_corpus/ through it - each one a regression test.
add_executable(fuzz_parser fuzz_parser.c)
target_link_libraries(fuzz_parser PRIVATE adifcore)
target_compile_options(fuzz_parser PRIVATE ${ADIFEDIT_C_FLAGS})
if(ADIFEDIT_FUZZ)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ADIFEDIT_FUZZ needs clang (libFuzzer); use the fuzz preset")
    endif()
    target_compile_definitions(fuzz_parser PRIVATE ADIFEDIT_LIBFUZZER)
    target_link_options(fuzz_parser PRIVATE -fsanitize=fuzzer)
else()
    add_test(NAME fuzz_corpus COMMAND fuzz_parser ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_corpus)
    set_tests_properties(fuzz_corpus PROPERTIES ENVIRONMENT "G_DEBUG=fatal-warnings")
endif()
