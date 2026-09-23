/* via6522.c — the 6522 VIA (design.md §7.3; scope in via6522.h). */

#include "via6522.h"

void via6522_reset(via6522_t *v) {
    /* A reset clears every register except the timers, the latches and
     * the shift register, which come up undefined; zero stands in. */
    *v = (via6522_t){0};
    v->in_a = v->in_b = 0xFFu;
    v->t1 = v->t2 = 0xFFFF;
}

static uint8_t port_in(uint8_t out, uint8_t ddr, uint8_t pins) {
    return (uint8_t)((out & ddr) | (pins & (uint8_t)~ddr));
}

uint8_t via6522_read(via6522_t *v, uint8_t reg) {
    switch (reg & 15u) {
    case VIA_ORB:    return port_in(v->orb, v->ddrb, v->in_b);
    case VIA_ORA:
    case VIA_ORA_NH: return port_in(v->ora, v->ddra, v->in_a);
    case VIA_DDRB:   return v->ddrb;
    case VIA_DDRA:   return v->ddra;
    case VIA_T1CL:
        v->ifr = (uint8_t)(v->ifr & ~VIA_INT_T1);
        return (uint8_t)v->t1;
    case VIA_T1CH:   return (uint8_t)((uint16_t)v->t1 >> 8);
    case VIA_T1LL:   return (uint8_t)v->t1_latch;
    case VIA_T1LH:   return (uint8_t)(v->t1_latch >> 8);
    case VIA_T2CL:
        v->ifr = (uint8_t)(v->ifr & ~VIA_INT_T2);
        return (uint8_t)v->t2;
    case VIA_T2CH:   return (uint8_t)((uint16_t)v->t2 >> 8);
    case VIA_SR:     return v->sr;
    case VIA_ACR:    return v->acr;
    case VIA_PCR:    return v->pcr;
    case VIA_IFR:    return (uint8_t)(v->ifr | (via6522_irq(v) ? VIA_INT_ANY : 0u));
    default:         return (uint8_t)(v->ier | 0x80u);   /* IER reads bit 7 set */
    }
}

void via6522_write(via6522_t *v, uint8_t reg, uint8_t val) {
    switch (reg & 15u) {
    case VIA_ORB:    v->orb = val; break;
    case VIA_ORA:
    case VIA_ORA_NH: v->ora = val; break;
    case VIA_DDRB:   v->ddrb = val; break;
    case VIA_DDRA:   v->ddra = val; break;
    case VIA_T1CL:
    case VIA_T1LL:
        v->t1_latch = (uint16_t)((v->t1_latch & 0xFF00u) | val);
        break;
    case VIA_T1CH:
        /* Latch high, then the whole latch into the counter: this is
         * the write that starts T1. */
        v->t1_latch = (uint16_t)((v->t1_latch & 0x00FFu) | (val << 8));
        v->t1 = v->t1_latch;
        v->t1_armed = true;
        v->ifr = (uint8_t)(v->ifr & ~VIA_INT_T1);
        break;
    case VIA_T1LH:
        v->t1_latch = (uint16_t)((v->t1_latch & 0x00FFu) | (val << 8));
        v->ifr = (uint8_t)(v->ifr & ~VIA_INT_T1);
        break;
    case VIA_T2CL:   v->t2_latch_lo = val; break;
    case VIA_T2CH:
        v->t2 = (int32_t)(v->t2_latch_lo | (val << 8));
        v->t2_armed = true;
        v->ifr = (uint8_t)(v->ifr & ~VIA_INT_T2);
        break;
    case VIA_SR:     v->sr = val; break;
    case VIA_ACR:    v->acr = val; break;
    case VIA_PCR:    v->pcr = val; break;
    case VIA_IFR:
        /* Writing a 1 clears that flag; bit 7 is not a flag. */
        v->ifr = (uint8_t)(v->ifr & ~val & 0x7Fu);
        break;
    default:
        /* IER: bit 7 says whether the other ones set or clear. */
        if (val & 0x80u) v->ier = (uint8_t)(v->ier | (val & 0x7Fu));
        else             v->ier = (uint8_t)(v->ier & ~val);
        break;
    }
}

void via6522_tick(via6522_t *v, uint32_t cycles) {
    v->t1 -= (int32_t)cycles;
    while (v->t1 < 0) {
        if (v->t1_armed) v->ifr |= VIA_INT_T1;
        if (v->acr & VIA_ACR_T1_FREERUN) {
            /* The counter holds -1 for a cycle and reloads on the next,
             * so the period is latch + 2 (6522 data sheet, T1 free-run). */
            v->t1 += (int32_t)v->t1_latch + 2;
        } else {
            v->t1_armed = false;
            v->t1 += 0x10000;      /* one-shot keeps counting, silently */
        }
    }

    /* Pulse-counting mode counts PB6 edges, and nothing drives PB6. */
    if (v->acr & VIA_ACR_T2_PULSES) return;
    v->t2 -= (int32_t)cycles;
    if (v->t2 < 0) {
        if (v->t2_armed) v->ifr |= VIA_INT_T2;
        v->t2_armed = false;
        v->t2 += 0x10000;
    }
}
