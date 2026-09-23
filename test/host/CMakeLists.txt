# Host tests (design.md §15.1). Every one of these builds src/core/ with
# the system compiler and no Pico SDK.

set(PICO_ATOM_HOST_TESTS
    test_m6502_cycles
    test_m6502_behaviour
    test_m6502_decimal
    test_bus
    test_i8255
    test_mc6847
    test_field
    test_present
    test_keymap
    test_via6522
    test_audio
    test_uef
    test_i8271
)

foreach(t ${PICO_ATOM_HOST_TESTS})
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE pico_atom_core)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${t} PRIVATE ${PICO_ATOM_WARNINGS})
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# test_audio measures pitch with lround and fabs.
target_link_libraries(test_audio PRIVATE m)

# The Dormann and Clark suites need binaries the repository does not ship;
# without them the test reports as skipped, not as passing.
add_executable(test_m6502_functional test_m6502_functional.c)
target_link_libraries(test_m6502_functional PRIVATE pico_atom_core)
target_include_directories(test_m6502_functional PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_m6502_functional PRIVATE ${PICO_ATOM_WARNINGS})
# PICO_ATOM_TEST_ROMS in the environment wins; otherwise the test looks
# where tools/fetch-test-suites.sh puts them by default.
target_compile_definitions(test_m6502_functional PRIVATE
    PICO_ATOM_DEFAULT_TEST_ROMS="${CMAKE_SOURCE_DIR}/test/suites"
)
add_test(NAME test_m6502_functional COMMAND test_m6502_functional)
set_tests_properties(test_m6502_functional PROPERTIES SKIP_RETURN_CODE 77)

# §15.1's golden images. The scenes are shared with vdg-ppm, which is
# what writes test/golden/, so the writer and the checker cannot drift.
add_library(vdg_scenes STATIC vdg_scenes.c)
target_link_libraries(vdg_scenes PUBLIC pico_atom_core)
target_include_directories(vdg_scenes PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(vdg_scenes PRIVATE ${PICO_ATOM_WARNINGS})

add_executable(test_mc6847_golden test_mc6847_golden.c)
target_link_libraries(test_mc6847_golden PRIVATE vdg_scenes)
target_compile_options(test_mc6847_golden PRIVATE ${PICO_ATOM_WARNINGS})
target_compile_definitions(test_mc6847_golden PRIVATE
    PICO_ATOM_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/test/golden"
)
add_test(NAME test_mc6847_golden COMMAND test_mc6847_golden)

# A tool, not a test: renders the VDG to PPM, both to verify the
# character ROM by eye (§16's prescribed method) and to regenerate the
# goldens above.
add_executable(vdg-ppm ${CMAKE_SOURCE_DIR}/tools/vdg-ppm.c)
target_link_libraries(vdg-ppm PRIVATE vdg_scenes)
target_compile_options(vdg-ppm PRIVATE ${PICO_ATOM_WARNINGS})

# tools/mkfont.py round trip. Guards a bug that silently dropped every
# glyph row beginning with a lit pixel, because '#' is both the lit-pixel
# character and the comment character.
find_package(Python3 COMPONENTS Interpreter)
if (Python3_FOUND)
    add_test(NAME test_mkfont
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/test_mkfont.py
                     ${CMAKE_SOURCE_DIR}/tools/mkfont.py)
endif()

# The real MOS on the host: a machine built from the user's ROM images,
# found by SHA-1 in PICO_ATOM_ROMS or the roms/ staging directory. Tests
# that need it report as skipped without them.
add_library(guest STATIC guest.c)
target_link_libraries(guest PUBLIC pico_atom_core)
target_include_directories(guest PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(guest PRIVATE ${PICO_ATOM_WARNINGS})
target_compile_definitions(guest PRIVATE
    PICO_ATOM_DEFAULT_ROMS="${CMAKE_SOURCE_DIR}/roms"
)

# M4: the real MOS boots and the keyboard types (§17). M6: the tape
# traps leave the machine as the ROM's own routines do (§11.2), and a
# restored snapshot runs on to the same state as the original (§11.5).
foreach(t test_boot test_tape test_snapshot test_cassette test_disc)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE guest)
    target_compile_options(${t} PRIVATE ${PICO_ATOM_WARNINGS})
    add_test(NAME ${t} COMMAND ${t})
    set_tests_properties(${t} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
target_link_libraries(test_boot PRIVATE m)

# test_keymap checks romset's SHA-1 table against README.md's list, which
# is the documented source of those digests (design.md §11.1).
target_compile_definitions(test_keymap PRIVATE
    PICO_ATOM_README="${CMAKE_SOURCE_DIR}/README.md"
)
