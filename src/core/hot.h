/* hot.h — which of the core's functions run from SRAM (design.md §6.3,
 * hardware-notes.md §9.2).
 *
 * On the device, flash code is fetched through a 16 KiB XIP cache that
 * both cores share, and the interpreter alone is larger than that. The
 * SDK's linker script copies every `.time_critical*` input section into
 * SRAM at boot, so naming a function's section is all it takes to move
 * it. That is the SDK's convention, not an SDK dependency: nothing here
 * includes a Pico header.
 *
 * Moves come in tiers, and PICO_ATOM_RAM_TIER says how many are taken.
 * Each tier was measured on the board (design.md §6.3 has the table),
 * and the firmware ships the one the curve chose (CMakeLists.txt). A
 * host build leaves it at 0 and gets ordinary functions.
 *
 *   ATOM_HOT1(name)  what the interpreter calls out to: the VIA tick and
 *                    IRQ line every instruction, the bus slow path, the
 *                    8255, the beeper. 4.4 KiB.
 *   ATOM_HOT2(name)  the interpreter and its loop. 20.6 KiB.
 *
 * A third tier, the cycle table, measured nothing and was dropped.
 * Whole files are the wrong unit (hardware-notes.md §9.2): mark the
 * function, not the module, and check the symbol moved from 0x1... to
 * 0x2... with arm-none-eabi-nm after the build.
 */
#ifndef PICO_ATOM_HOT_H
#define PICO_ATOM_HOT_H

#ifndef PICO_ATOM_RAM_TIER
#define PICO_ATOM_RAM_TIER 0
#endif

#define ATOM_IN_RAM_(name) __attribute__((section(".time_critical.atom_" #name))) name

#if PICO_ATOM_RAM_TIER >= 1
#define ATOM_HOT1(name) ATOM_IN_RAM_(name)
#else
#define ATOM_HOT1(name) name
#endif

#if PICO_ATOM_RAM_TIER >= 2
#define ATOM_HOT2(name) ATOM_IN_RAM_(name)
#else
#define ATOM_HOT2(name) name
#endif

#endif /* PICO_ATOM_HOT_H */
