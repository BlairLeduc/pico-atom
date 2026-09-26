/* atomulator-trace.c — Atomulator's 6502, 8255, VIA and 8271, headless,
 * printing one line per instruction: the reference half of §15.1's
 * trace-diff harness (design.md). tools/trace/build-atomulator.sh
 * compiles it against an Atomulator checkout; nothing of Atomulator is in
 * this tree.
 *
 *   atomulator-trace KERNEL BASIC FLOAT DOS [-n INSNS] [-f FIELDS] [-k KEYS]
 *
 * Each line is the state before an instruction: PC A X Y S P, cycles, and
 * the three bytes at PC, the same as atom-trace prints. Keys are pressed
 * at an instruction boundary, by guest cycle (keyscript.h). Interrupt entry is not an instruction and has
 * no line; the handler's first instruction does. P has bits 4 and 5 set,
 * because Atomulator keeps neither.
 *
 * The machine is the one atom-trace builds by default: no RAMROM, RAM
 * from #0000 to #7FFF bar the 8271's page, the 8271, the VIA, no utility
 * ROM, zero RAM and so a zero RND seed, and NTSC's 262 lines. The clock
 * is Atomulator's own: 64 cycles a line, 16,768 a field, against the
 * VDG's 16,667, so FS drifts between the two and polling loops run
 * different counts. trace-diff.py resyncs across those.
 */

#include <allegro.h>

#include "atom.h"
#include "roms.h"
#include "keyscript.h"

/* ---- what Atomulator's other files would have defined --------------- */

volatile char key[KEY_MAX];
int keylookup[128];
JOYSTICK_INFO joy[1];
int joy_left, joy_right, joy_up, joy_down;
int keyjoyst;

int main_ramflag = 4;      /* everything up to #7FFF but the FDC's page */
int vid_ramflag  = 7;
int vid_top;
int ramrom_enable = 0;
int fdc1770 = 0, GD_bank = 0;
int sndatomsid = 0;
int snow = 0;
int cswena = 0;
int spon, tpon;
int debugon = 1;           /* makes exec6502 call dodebugger per instruction */
int fetchc[65536], readc[65536], writec[65536];

int motoron, motorspin, fdctime, disctime, curdrive;
void (*fdccallback)(void);
void (*fdcdata)(uint8_t dat);
void (*fdcspindown)(void);
void (*fdcfinishread)(void);
void (*fdcnotfound)(void);
void (*fdcdatacrcerror)(void);
void (*fdcheadercrcerror)(void);
void (*fdcwriteprotect)(void);
int  (*fdcgetdata)(int last);

void rpclog(char *format, ...) { (void)format; }
void prtbuf(char *format, ...) { (void)format; }
void debuglog(char *format, ...) { (void)format; }
void debugread(uint16_t addr) { (void)addr; }
void debugwrite(uint16_t addr, uint8_t val) { (void)addr; (void)val; }
uint8_t ReadMMC(uint16_t addr) { return (uint8_t)(addr >> 8); }
void WriteMMC(uint16_t addr, uint8_t val) { (void)addr; (void)val; }
uint8_t read1770(uint16_t addr) { (void)addr; return 0xFF; }
void write1770(uint16_t addr, uint8_t val) { (void)addr; (void)val; }
uint8_t sid_read(int addr) { (void)addr; return 0; }
void sid_write(int addr, uint8_t val) { (void)addr; (void)val; }
void sid_fillbuf(int16_t *buf, int len) { (void)buf; (void)len; }
void givealbuffer(int16_t *buf) { (void)buf; }
int  getcsw(void) { return 0; }
void polluef(void) {}
void disc_poll(void) {}
void disc_seek(int d, int t) { (void)d; (void)t; }
void disc_readsector(int d, int s, int t, int si, int de) { (void)d; (void)s; (void)t; (void)si; (void)de; }
void disc_writesector(int d, int s, int t, int si, int de) { (void)d; (void)s; (void)t; (void)si; (void)de; }
void disc_readaddress(int d, int t, int s, int de) { (void)d; (void)t; (void)s; (void)de; }
void disc_format(int d, int t, int s, int de) { (void)d; (void)t; (void)s; (void)de; }

/* video.c's drawline, less the drawing: FS (vbl) low for lines 192-223. */
void drawline(int line) {
    if (line == 0)   vbl = 0;
    if (line == 192) vbl = 1;
    if (line == 224) vbl = 0;
}

/* ---- from Atomulator ------------------------------------------------ */

extern uint8_t *ram, *rom;
extern int totcyc;
extern int keys[16][6];
void initmem(void);
void reset_rom(void);
void init8255(void);
void resetvia(void);
void reset8271(void);
uint8_t readmeml(uint16_t addr);

/* ---- the trace ------------------------------------------------------ */

static unsigned long long insns, max_insns = 2000000ull;
static keyscript_t ks;
static void press(const ks_event_t *e);

void dodebugger(int linenum) {
    (void)linenum;
    const ks_event_t *e;
    while ((e = ks_due(&ks, (unsigned long long)totcyc)) != NULL) press(e);

    uint8_t pf = 0x30;
    if (p.c) pf |= 0x01;
    if (p.z) pf |= 0x02;
    if (p.i) pf |= 0x04;
    if (p.d) pf |= 0x08;
    if (p.v) pf |= 0x40;
    if (p.n) pf |= 0x80;
    /* readmeml has no side effects outside I/O, and code never runs there. */
    printf("%04X %02X %02X %02X %02X %02X %d %02X%02X%02X\n", pc, a, x, y, s, pf, totcyc,
           readmeml(pc), readmeml((uint16_t)(pc + 1)), readmeml((uint16_t)(pc + 2)));
    if (++insns >= max_insns) { fflush(stdout); exit(0); }
}

static int load(const char *path, int ofs) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "%s: cannot open\n", path); return 0; }
    size_t n = fread(&rom[ofs], 1, ROM_SIZE_ATOM, f);
    fclose(f);
    if (n != ROM_SIZE_ATOM) { fprintf(stderr, "%s: not 4 KiB\n", path); return 0; }
    return 1;
}

static void press(const ks_event_t *e) {
    int k = 0;
    switch (e->kind) {
    case KS_CELL:  k = keys[e->col][e->bit]; break;
    case KS_SHIFT: k = KEY_LSHIFT; break;
    case KS_CTRL:  k = KEY_LCONTROL; break;
    case KS_REPT:  k = KEY_ALT; break;
    }
    if (k == 0) {
        fprintf(stderr, "no Atomulator key at column %u bit %u\n", e->col, e->bit);
        exit(2);
    }
    key[k] = e->down ? 1 : 0;
}

int main(int argc, char **argv) {
    const char *keyfile = NULL;
    unsigned long max_fields = 0;
    const char *romv[4];
    int nrom = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc)      max_insns  = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) max_fields = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) keyfile    = argv[++i];
        else if (nrom < 4)                               romv[nrom++] = argv[i];
        else { fprintf(stderr, "usage: %s KERNEL BASIC FLOAT DOS [-n INSNS] [-f FIELDS] [-k KEYS]\n", argv[0]); return 2; }
    }
    if (nrom != 4) { fprintf(stderr, "usage: %s KERNEL BASIC FLOAT DOS [-n INSNS] [-f FIELDS] [-k KEYS]\n", argv[0]); return 2; }
    if (!ks_load(&ks, keyfile)) return 2;

    for (int c = 0; c < 128; c++) keylookup[c] = c;
    SET_VID_TOP();
    initmem();
    memset(ram, 0, 0x10000);       /* initmem seeds RND from rand() */
    /* The empty utility socket: atom-trace reads open bus there, the last
     * byte on the bus, which for an absolute load is the address's high
     * byte (bus.c). The floating-point ROM looks at #A000 for a ROM. */
    memset(rom, 0, ROM_MEM_SIZE);
    for (int i = 0; i < ROM_SIZE_ATOM; i++) rom[ROM_OFS_UTILITY + i] = (uint8_t)(0xA0 | (i >> 8));
    if (!load(romv[0], ROM_OFS_AKERNEL) || !load(romv[1], ROM_OFS_ABASIC) ||
        !load(romv[2], ROM_OFS_AFLOAT)  || !load(romv[3], ROM_OFS_DOSROM)) return 2;

    init8255();
    resetvia();
    reset8271();
    reset6502();

    for (unsigned long field = 0; !max_fields || field < max_fields; field++) exec6502(262, 64);
    fflush(stdout);
    return 0;
}
