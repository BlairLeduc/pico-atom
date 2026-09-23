/* tape.h — tape phase 1: OSLOAD and OSSAVE served by trapping the MOS
 * (design.md §11.2).
 *
 * The kernel's OSLOAD (#FFE0) and OSSAVE (#FFDD) are JMP (#020C) and
 * JMP (#020E), and reset points those vectors at #F96E and #FAE5 — read
 * off the kernel ROM, which is how §16's row was settled. The trap is on
 * the two handlers, not on the #FFxx entries, so anything that repoints
 * a vector (AtomDOS, a utility ROM, a game's own loader) never reaches
 * it: that is the page-2 variant §11.2 prefers, for free.
 *
 * When the PC arrives at a handler whose bytes match the stock kernel,
 * the CPU stalls there the way a 6502 with RDY held low does — guest time
 * passes, devices tick, no instruction runs — and the request waits in
 * atom_t.tape for the port. The port finds the file and hands the bytes
 * over; the core then leaves page zero as the ROM routine would have and
 * executes the RTS. A port that cannot serve a request declines it, and
 * the ROM routine runs as though no trap existed.
 *
 * Files are ATM: a 22-byte header — a 16-byte name, then load address,
 * execution address and length, little-endian — and the data.
 */
#ifndef PICO_ATOM_TAPE_H
#define PICO_ATOM_TAPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

struct atom_s;

/* The stock kernel's handlers (§11.2, §16). */
#define TAPE_OSLOAD_PC  0xF96Eu
#define TAPE_OSSAVE_PC  0xFAE5u

/* The deck's cues in the same kernel (§11.3): the prompt routine, entered
 * with A = 4 for PLAY TAPE, 5 for REWIND and 6 for RECORD; the point
 * after it has read the key that answers it; and OSLOAD's one exit,
 * shared by the named and nameless paths. A *RUN's OSLOAD returns to
 * TAPE_RUN_RETURN + 1. */
#define TAPE_PROMPT_PC   0xFC40u
#define TAPE_ANSWERED_PC 0xFC79u
#define TAPE_LOADED_PC   0xF953u
#define TAPE_RUN_RETURN  0xFA22u
#define TAPE_PROMPT_PLAY 4u

/* The MOS takes a name of at most 13 characters and a CR; a longer one
 * is its NAME error (#F85C), which the trap leaves to the ROM. */
#define TAPE_NAME_MAX   13u

#define ATM_HEADER_LEN  22u

typedef struct {
    char     name[ATOM_ATM_NAME_LEN + 1];   /* NUL-terminated */
    uint16_t load;
    uint16_t exec;
    uint16_t len;
} atm_header_t;

void atm_header_decode(const uint8_t raw[ATM_HEADER_LEN], atm_header_t *h);
void atm_header_encode(const atm_header_t *h, uint8_t raw[ATM_HEADER_LEN]);

/* Does this file answer to the name the guest asked for? The MOS
 * compares byte for byte (#FBD5), so this does too. */
bool atm_name_matches(const atm_header_t *h, const char *want);

typedef enum {
    TAPE_NONE = 0,
    TAPE_LOAD,
    TAPE_SAVE,
} tape_op_t;

typedef struct {
    tape_op_t op;          /* the request the CPU is stalled on          */
    char      name[TAPE_NAME_MAX + 1];      /* as the guest gave it      */

    /* The guest's parameter block, the ten bytes at X (#F84F copies
     * them to #C9-#D2 before anything else). */
    uint8_t   block[10];

    /* TAPE_LOAD: where the guest wants the data. Bit 7 of the block's
     * fifth byte clear means "at the file's own address" (#FC2B). */
    bool      own_addr;
    uint16_t  addr;

    /* A load in progress: the file, where it is going, how far. */
    atm_header_t hdr;
    uint16_t  base;
    uint32_t  done;
    /* The last block's data bytes summed as they arrive: the MOS sums
     * what it reads off tape (#FC23), not what the write left behind,
     * which differs over ROM or I/O. */
    uint32_t  sum_from;
    uint8_t   data_sum;

    bool      pass;        /* declined: let the ROM run this call once   */
    bool      cue_play;    /* PLAY TAPE is up: its key starts the deck    */
    uint32_t  served;      /* requests completed, for the heartbeat      */
} tape_t;

/* Called by atom_run when the PC may be one of the addresses above. True
 * if the CPU is to stall this instruction boundary, which only a trap
 * does; the deck's cues start and stop the cassette and return false. */
bool tape_at(struct atom_s *m);
bool tape_trap(struct atom_s *m);

/* The request the CPU is stalled on, or NULL. */
const tape_t *atom_tape_pending(const struct atom_s *m);

/* No file: run the ROM routine instead, which prompts and reads the
 * cassette input as the real machine would. */
void atom_tape_decline(struct atom_s *m);

/* Serving a TAPE_LOAD: begin with the file's header, which returns the
 * address the data will land at; write the data in as many pieces as
 * suits the port; end. Writes go through the bus, so a file that runs
 * into ROM or I/O does what the ROM's STA would. */
uint16_t atom_tape_load_begin(struct atom_s *m, const atm_header_t *h);
void     atom_tape_load_data(struct atom_s *m, const uint8_t *src, size_t n);
void     atom_tape_load_end(struct atom_s *m);

/* Serving a TAPE_SAVE: the header the file should carry, then its data
 * read from guest memory without side effects, then end. */
void   atom_tape_save_header(const struct atom_s *m, atm_header_t *h);
size_t atom_tape_save_data(const struct atom_s *m, uint32_t offset, uint8_t *dst, size_t n);
void   atom_tape_save_end(struct atom_s *m);

#endif /* PICO_ATOM_TAPE_H */
