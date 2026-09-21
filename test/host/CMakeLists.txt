# Host tests (design.md §15.1). Every one of these builds src/core/ with
# the system compiler and no Pico SDK.

set(PICO_ATOM_HOST_TESTS
    test_m6502_cycles
    test_m6502_behaviour
    test_m6502_decimal
    test_bus
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
