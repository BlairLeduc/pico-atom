/* bus.h — guest memory dispatch (design.md §7).
 *
 * Fast path is a single indexed load or store through a 256-entry page
 * table; everything else falls into bus_read_slow / bus_write_slow.
 */
#ifndef PICO_ATOM_BUS_H
#define PICO_ATOM_BUS_H

#include <stdint.h>

#include "atom.h"

uint8_t bus_read_slow(atom_t *m, uint16_t a);
void    bus_write_slow(atom_t *m, uint16_t a, uint8_t v);

static inline uint8_t bus_read(atom_t *m, uint16_t a) {
    const page_t *p = &m->page[a >> 8];
    if (__builtin_expect(p->read != NULL, 1)) {
        uint8_t v = p->read[a & 0xFFu];
        m->open_bus = v;
        return v;
    }
    return bus_read_slow(m, a);
}

static inline void bus_write(atom_t *m, uint16_t a, uint8_t v) {
    page_t *p = &m->page[a >> 8];
    m->open_bus = v;
    if (__builtin_expect(p->write != NULL, 1)) {
        p->write[a & 0xFFu] = v;      /* RAM, including VRAM */
    } else {
        bus_write_slow(m, a, v);      /* ROM (ignored) or I/O */
    }
}

#endif /* PICO_ATOM_BUS_H */
