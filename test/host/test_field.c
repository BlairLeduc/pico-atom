/* test_field.c — the core 0 slice loop, split where FS falls and rises
 * (design.md §12.1).
 *
 * §12.1 asks for exactly this: a guest loop polling FS must both observe
 * the low state and escape it. Asserting that the accessor flips the bit
 * does not catch the bug this guards — pulsing FS after the whole field
 * has run, so no instruction ever executes while it is low — so the test
 * executes a polling loop and counts what it saw.
 */

#include "atom.h"
#include "bus.h"
#include "test_util.h"

static atom_t g_machine;

/* #1000  LDA #B002 ; BMI #1000   wait for FS low (bit 7 clear)
 * #1005  INC #70                 saw it low
 * #1007  LDA #B002 ; BPL #1007   wait for FS high again
 * #100C  INC #71                 escaped
 * #100E  JMP #1000 */
static const uint8_t poll_fs[] = {
    0xAD, 0x02, 0xB0,  0x30, 0xFB,
    0xE6, 0x70,
    0xAD, 0x02, 0xB0,  0x10, 0xFB,
    0xE6, 0x71,
    0x4C, 0x00, 0x10,
};

/* A game's frame, the shape of one that XOR-erases its sprites and draws
 * them again after FS falls: that leaves the screen half drawn for as
 * long as the redraw takes, which the beam never shows on an Atom.
 * #1000  LDA #B002 ; BMI #1000   wait for FS low
 * #1005  INC #70                 frame begun
 * #1007  LDY #3                  about 3,000 cycles of redraw: past FS's
 * #1009  LDX #200                rising edge, inside the blank lines
 * #100B  DEX ; BNE #100B
 * #100E  DEY ; BNE #1009
 * #1011  INC #72                 frame drawn
 * #1013  LDA #B002 ; BPL #1013   wait for FS high
 * #1018  JMP #1000 */
static const uint8_t redraw[] = {
    0xAD, 0x02, 0xB0,  0x30, 0xFB,
    0xE6, 0x70,
    0xA0, 0x03,
    0xA2, 0xC8,
    0xCA,  0xD0, 0xFD,
    0x88,  0xD0, 0xF8,
    0xE6, 0x72,
    0xAD, 0x02, 0xB0,  0x10, 0xFB,
    0x4C, 0x00, 0x10,
};

static atom_t *load(const uint8_t *code, unsigned len) {
    atom_config_t cfg;
    atom_config_default(&cfg);
    atom_init(&g_machine, &cfg);
    for (unsigned i = 0; i < len; i++)
        bus_write(&g_machine, (uint16_t)(0x1000u + i), code[i]);
    g_machine.cpu.pc = 0x1000;
    return &g_machine;
}

static atom_t *machine(void) { return load(poll_fs, sizeof poll_fs); }

int main(void) {
    const unsigned fields = 100;

    /* ---- the real loop: FS is observed low once per field ------------ */
    {
        atom_t *m = machine();
        for (unsigned f = 0; f < fields; f++) atom_run_field(m);

        CHECK(m->ram[0x70] == fields,
              "the guest should see FS low once per field, saw it %u times in %u",
              m->ram[0x70], fields);
        /* The blank lines run after FS rises, so every field's escape
         * happens inside it. */
        CHECK(m->ram[0x71] == fields,
              "the guest should escape each flyback, escaped %u times in %u",
              m->ram[0x71], fields);
        CHECK((bus_read(m, 0xB002) & 0x80u) != 0, "FS should be high between fields");
    }

    /* ---- debt carries across both halves and across fields ----------- */
    {
        atom_t *m = machine();
        uint64_t want = (uint64_t)fields * ATOM_CYCLES_PER_FIELD;
        uint64_t start = m->cpu.cycles;   /* the reset sequence counts too */
        uint64_t ran = 0;
        for (unsigned f = 0; f < fields; f++) ran += atom_run_field(m);

        /* Every instruction here is 7 cycles or fewer, so after any number
         * of fields the machine is at most one instruction ahead. */
        CHECK(ran == m->cpu.cycles - start, "atom_run_field should report what it ran");
        CHECK(ran >= want && ran - want < 7u,
              "%u fields should run %llu cycles give or take one instruction, ran %llu",
              fields, (unsigned long long)want, (unsigned long long)ran);
    }

    /* ---- control: the M0 pulse shape really is the bug --------------- *
     * Run the whole field, then pulse FS with nothing executing while it
     * is low. If this ever starts passing the check above, the test has
     * stopped being able to tell the two apart. */
    {
        atom_t *m = machine();
        for (unsigned f = 0; f < fields; f++) {
            m->budget += (int32_t)ATOM_CYCLES_PER_FIELD;
            if (m->budget > 0) m->budget -= (int32_t)atom_run(m, (uint32_t)m->budget);
            atom_field_sync(m, true);
            atom_field_sync(m, false);
        }
        CHECK(m->ram[0x70] == 0,
              "a back-to-back FS pulse should be unobservable, but the guest saw it %u times",
              m->ram[0x70]);
    }

    /* ---- instructions are counted, for §12.3's host cycles each ------ *
     * #2000  NOP ; NOP ; JMP #2000   seven cycles, three instructions */
    {
        atom_t *m = machine();
        static const uint8_t loop[] = { 0xEA, 0xEA, 0x4C, 0x00, 0x20 };
        for (unsigned i = 0; i < sizeof(loop); i++)
            bus_write(m, (uint16_t)(0x2000u + i), loop[i]);
        m->cpu.pc = 0x2000;
        m->instructions = 0;
        uint32_t ran = atom_run(m, 7000u);
        CHECK(ran == 7000u, "the loop should end on a boundary, ran %u", (unsigned)ran);
        CHECK(m->instructions == 3000u,
              "7,000 cycles of the loop are 3,000 instructions, counted %llu",
              (unsigned long long)m->instructions);
    }

    /* ---- the field ends where the next active line begins ------------ *
     * So a snapshot taken when atom_run_field returns sees a redraw that
     * began when FS fell finished, as the beam would. */
    {
        atom_t *m = load(redraw, sizeof redraw);
        unsigned torn = 0;
        for (unsigned f = 0; f < fields; f++) {
            atom_run_field(m);
            if (m->ram[0x72] != m->ram[0x70]) torn++;
        }
        CHECK(m->ram[0x70] == fields, "the frame should begin once per field, began %u times",
              m->ram[0x70]);
        CHECK(torn == 0, "%u of %u fields ended with the redraw half done", torn, fields);
    }

    /* ---- control: ending the field at FS's rising edge tears --------- *
     * The split before §12.1 took the line counts. If this stops seeing
     * torn fields, the redraw above no longer runs past FS's rise and
     * the check above proves nothing. */
    {
        atom_t *m = load(redraw, sizeof redraw);
        unsigned torn = 0;
        for (unsigned f = 0; f < fields; f++) {
            m->budget += (int32_t)(ATOM_CYCLES_PER_FIELD - 999u);
            if (m->budget > 0) m->budget -= (int32_t)atom_run(m, (uint32_t)m->budget);
            atom_field_sync(m, true);
            m->budget += 999;
            if (m->budget > 0) m->budget -= (int32_t)atom_run(m, (uint32_t)m->budget);
            atom_field_sync(m, false);
            if (m->ram[0x72] != m->ram[0x70]) torn++;
        }
        CHECK(torn > fields / 2, "the old split should tear the redraw, tore %u of %u",
              torn, fields);
    }

    /* ---- the parts are §12.1's line counts --------------------------- */
    CHECK(ATOM_BLANK_LINES == 38u, "38 lines of blank and top border, got %u",
          (unsigned)ATOM_BLANK_LINES);
    CHECK(ATOM_ACTIVE_CYCLES == 12214u && ATOM_FS_LOW_CYCLES == 2035u &&
          ATOM_BLANK_CYCLES == 2417u,
          "192, 32 and 38 of 262 lines should be 12214, 2035 and 2417 cycles, got %u, %u, %u",
          (unsigned)ATOM_ACTIVE_CYCLES, (unsigned)ATOM_FS_LOW_CYCLES,
          (unsigned)ATOM_BLANK_CYCLES);

    TEST_DONE();
}
