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
)

foreach(t ${PICO_ATOM_HOST_TESTS})
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE pico_atom_core)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${t} PRIVATE ${PICO_ATOM_WARNINGS})
    add_test(NAME ${t} COMMAND ${t})
endforeach()

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

# M4: the real MOS boots and the keyboard types (§17). Needs the user's
# ROM images, found by SHA-1 in PICO_ATOM_ROMS or the roms/ staging
# directory; without them it reports as skipped.
add_executable(test_boot test_boot.c)
target_link_libraries(test_boot PRIVATE pico_atom_core)
target_include_directories(test_boot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_boot PRIVATE ${PICO_ATOM_WARNINGS})
target_compile_definitions(test_boot PRIVATE
    PICO_ATOM_DEFAULT_ROMS="${CMAKE_SOURCE_DIR}/roms"
)
add_test(NAME test_boot COMMAND test_boot)
set_tests_properties(test_boot PROPERTIES SKIP_RETURN_CODE 77)
