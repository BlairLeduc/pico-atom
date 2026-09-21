/* test_bus.c — page table, open bus, ROM write protection and the two
 * places the address map is easy to get wrong (design.md §7).
 */

#include <string.h>

#include "atom.h"
#include "bus.h"
#include "test_util.h"

static atom_t g_machine;

static atom_t *machine(const atom_config_t *cfg) {
    atom_init(&g_machine, cfg);
    return &g_machine;
}

int main(void) {
    atom_config_t cfg;
    atom_config_default(&cfg);

    /* ---- the default machine is fully expanded (§7.2) ---------------- */
    {
        atom_t *m = machine(&cfg);
        bus_write(m, 0x0000, 0x11);   /* zero page      */
        bus_write(m, 0x3FFF, 0x22);   /* top of text    */
        bus_write(m, 0x8000, 0x33);   /* VRAM           */
        bus_write(m, 0x97FF, 0x44);   /* top of VRAM    */
        CHECK(bus_read(m, 0x0000) == 0x11, "#0000 should be RAM");
        CHECK(bus_read(m, 0x3FFF) == 0x22, "#3FFF should be RAM");
        CHECK(bus_read(m, 0x8000) == 0x33, "#8000 should be VRAM");
        CHECK(bus_read(m, 0x97FF) == 0x44, "#97FF should be VRAM");
        CHECK((m->page_flags[0x80] & PAGE_VRAM) != 0, "#8000 should be flagged VRAM");
    }

    /* ---- the video aperture is absent on a stock machine ------------- */
    {
        atom_t *m = machine(&cfg);
        bus_write(m, 0x4000, 0x5A);            /* unpopulated  */
        /* Open bus is the last value on the bus, not 0xFF (§7.2). */
        CHECK(bus_read(m, 0x4000) == 0x5A, "an unpopulated read returns the open bus");
        bus_write(m, 0x0000, 0xA5);
        CHECK(bus_read(m, 0x9800) == 0xA5,
              "#9800 is unpopulated by default and follows the open bus");

        atom_config_t big = cfg;
        big.video_aperture = true;
        m = machine(&big);
        bus_write(m, 0x9800, 0x77);
        CHECK(bus_read(m, 0x9800) == 0x77, "the aperture is RAM when configured present");
    }

    /* ---- ROM: readable, writes discarded ----------------------------- */
    {
        atom_t *m = machine(&cfg);
        uint8_t image[512];
        for (size_t i = 0; i < sizeof(image); i++) image[i] = (uint8_t)i;
        CHECK(atom_load_rom(m, 0xC000, image, sizeof(image)), "ROM load should succeed");
        CHECK(bus_read(m, 0xC000) == 0x00 && bus_read(m, 0xC0FF) == 0xFF,
              "the ROM image should read back");
        bus_write(m, 0xC010, 0xEE);
        CHECK(bus_read(m, 0xC010) == 0x10, "a write to ROM must be discarded");
        CHECK((m->page_flags[0xC0] & PAGE_ROM) != 0, "ROM pages should be flagged");

        CHECK(!atom_load_rom(m, 0xC001, image, sizeof(image)),
              "a misaligned ROM load should be rejected");
        CHECK(!atom_load_rom(m, 0xFF00, image, sizeof(image)),
              "a ROM load running off the top of the map should be rejected");
    }

    /* ---- #B000-#BFFF is I/O throughout and mirrors every four bytes -- */
    {
        atom_t *m = machine(&cfg);
        for (unsigned p = 0xB0; p <= 0xBF; p++) {
            CHECK((m->page_flags[p] & PAGE_IO) != 0,
                  "page #%02X should be I/O, flags 0x%02X", p, m->page_flags[p]);
            CHECK(m->page[p].write == NULL,
                  "page #%02X must have no fast write path or the device is bypassed", p);
        }
        /* A write into the 8255 block must not become RAM behind our
         * backs: #B000 and its #B004 mirror both reach the device, and
         * neither leaves a byte in ram[]. */
        m->ram[0xB000] = 0x00;
        m->ram[0xB004] = 0x00;
        bus_write(m, 0xB000, 0x5C);
        bus_write(m, 0xB004, 0x5C);
        CHECK(m->ram[0xB000] == 0x00 && m->ram[0xB004] == 0x00,
              "an 8255 write must not fall through to RAM");
    }

    /* ---- the 8271 hides inside RAM page #0A (§7.1, §7.3) ------------- */
    {
        atom_config_t dos = cfg;
        dos.atomdos = true;
        atom_t *m = machine(&dos);

        CHECK((m->page_flags[0x0A] & PAGE_IO) != 0,
              "with AtomDOS, page #0A must lose its fast path");
        CHECK(m->page[0x0A].write == NULL,
              "if page #0A kept a fast write the FDC would never be reached");

        /* The four FDC bytes are not RAM ... */
        m->ram[0x0A01] = 0x00;
        bus_write(m, 0x0A01, 0x9E);
        CHECK(m->ram[0x0A01] == 0x00, "#0A01 belongs to the FDC, not to RAM");

        /* ... and the other 252 bytes of the page still are. */
        bus_write(m, 0x0A04, 0x9E);
        CHECK(bus_read(m, 0x0A04) == 0x9E, "#0A04 should still be ordinary RAM");
        bus_write(m, 0x0AFF, 0x3C);
        CHECK(bus_read(m, 0x0AFF) == 0x3C, "#0AFF should still be ordinary RAM");

        /* Without AtomDOS the whole page is plain RAM on the fast path. */
        m = machine(&cfg);
        CHECK(m->page[0x0A].write != NULL, "without AtomDOS page #0A is ordinary RAM");
        bus_write(m, 0x0A01, 0x9E);
        CHECK(bus_read(m, 0x0A01) == 0x9E, "#0A01 is RAM on a machine with no disc system");
    }

    /* ---- atom_map_ram rejects a zero length -------------------------- */
    {
        atom_t *m = machine(&cfg);
        /* #4000 is unpopulated by default; a zero-length map at #0000
         * must not quietly populate it (or anything else). */
        atom_map_ram(m, 0x0000, 0);
        CHECK(m->page[0x40].write == NULL,
              "atom_map_ram(m, 0, 0) must map nothing, not the whole address space");
        CHECK((m->page_flags[0x40] & PAGE_OPEN) != 0, "#4000 should still be unpopulated");

        /* A length running off the top of the map is clamped, not wrapped. */
        m = machine(&cfg);
        atom_map_ram(m, 0xFF00, 0x1000);
        CHECK(m->page[0xFF].write != NULL, "#FF00 should be mapped");
        CHECK(m->page[0x00].write != NULL, "#0000 is RAM on the default machine");

        /* One page is one page. */
        m = machine(&cfg);
        atom_map_ram(m, 0x4000, 1);
        CHECK(m->page[0x40].write != NULL, "a one-byte map should cover its page");
        CHECK(m->page[0x41].write == NULL, "a one-byte map must not spill into #4100");
    }

    /* ---- the field rate is configuration, not a constant (§18) ------- */
    {
        atom_config_t fifty = cfg;
        fifty.field_hz = 50;
        atom_t *m = machine(&fifty);
        CHECK(atom_cycles_per_field(m) == 20000, "50 Hz should give 20000 cycles per field");
        m = machine(&cfg);
        CHECK(atom_cycles_per_field(m) == 16666, "60 Hz should give 16666 cycles per field");

        /* field_hz is public configuration, so it can arrive as zero.
         * That must not divide by zero — atom_init sanitises it, and the
         * accessor guards the divide for anything that reaches into cfg
         * afterwards. */
        atom_config_t zero = cfg;
        zero.field_hz = 0;
        m = machine(&zero);
        CHECK(m->cfg.field_hz == ATOM_FIELD_HZ_DEFAULT,
              "atom_init should sanitise a zero field rate, got %u", m->cfg.field_hz);
        CHECK(atom_cycles_per_field(m) == 16666, "a sanitised field rate should still divide");

        m->cfg.field_hz = 0;   /* reached in behind atom_init's back */
        CHECK(atom_cycles_per_field(m) == 16666,
              "atom_cycles_per_field must not divide by zero");
    }

    TEST_DONE();
}
