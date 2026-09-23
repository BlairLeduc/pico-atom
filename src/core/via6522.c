/* via6522.c — the 6522 VIA (design.md §7.3, §7.4; scope in via6522.h). */

#include "via6522.h"

#include "hot.h"

/* The PCR's CA2 or CB2 field, bits 3:1 or 7:5: 0-3 are inputs (bit 1
 * the positive edge, bit 0 an independent interrupt that a port access
 * leaves alone), 4 handshake, 5 pulse, 6 held low, 7 held high. */
#define C2_HANDSHAKE 4u
#define C2_PULSE     5u
#define C2_LOW       6u
#define C2_HIGH      7u

static inline uint8_t ca2_mode(const via6522_t *v) { return (uint8_t)((v->pcr >> 1) & 7u); }
static inline uint8_t cb2_mode(const via6522_t *v) { return (uint8_t)((v->pcr >> 5) & 7u); }

/* An input mode's flag survives a port access only when independent. */
static inline bool c2_cleared_by_port(uint8_t mode) { return (mode & 5u) != 1u; }

/* CB1 is the shift clock's output whenever the chip makes the clock. */
static inline bool sr_clocks_cb1(uint8_t mode) {
    return mode != VIA_SR_OFF && mode != VIA_SR_IN_EXT && mode != VIA_SR_OUT_EXT;
}
static inline bool sr_drives_cb2(uint8_t mode) { return mode >= VIA_SR_OUT_FREE; }

void via6522_reset(via6522_t *v) {
    /* A reset clears every register except the timers, the latches and
     * the shift register, which come up undefined; zero stands in. The
     * lines idle high, pulled up. */
    *v = (via6522_t){0};
    v->in_a = v->in_b = 0xFFu;
    v->t1 = v->t2 = 0xFFFF;
    v->pb7 = true;
    v->ca1 = v->ca2 = v->cb1 = v->cb2 = true;
}

static uint8_t port_in(uint8_t out, uint8_t ddr, uint8_t pins) {
    return (uint8_t)((out & ddr) | (pins & (uint8_t)~ddr));
}

/* ---- the shift register ------------------------------------------------ */

/* CB1 half-cycles take T2's low latch + 2 cycles, or one cycle of Φ2
 * (§16: the rate of the Φ2 modes). */
static int16_t sr_half_period(const via6522_t *v) {
    uint8_t mode = via6522_sr_mode(v);
    return (mode == VIA_SR_IN_PHI2 || mode == VIA_SR_OUT_PHI2)
               ? 1 : (int16_t)(v->t2_latch_lo + 2u);
}

/* One edge of the shift clock: data goes out on the falling edge, most
 * significant bit first, and comes round into bit 0; it comes in off
 * CB2 on the rising edge. Eight bits raise the flag, except that the
 * free-running mode recirculates for ever without one. */
static void sr_edge(via6522_t *v, bool rising) {
    uint8_t mode = via6522_sr_mode(v);
    if (!rising && sr_drives_cb2(mode)) {
        v->cb2 = (v->sr & 0x80u) != 0;
        v->sr = (uint8_t)((v->sr << 1) | (v->sr >> 7));
    } else if (rising && !sr_drives_cb2(mode)) {
        v->sr = (uint8_t)((v->sr << 1) | (v->cb2 ? 1u : 0u));
    }
    if (--v->sr_halves == 0) {
        if (mode == VIA_SR_OUT_FREE) v->sr_halves = 16;
        else v->ifr |= VIA_INT_SR;
    }
}

/* Reading or writing the register starts eight more bits. */
static void sr_start(via6522_t *v) {
    v->ifr = (uint8_t)(v->ifr & ~VIA_INT_SR);
    if (via6522_sr_mode(v) == VIA_SR_OFF) return;
    v->sr_halves = 16;
    v->sr_timer = sr_half_period(v);
}

/* A tape stall passes a whole slice in one tick (§11.2), so the sum is
 * kept wide; a byte is at most 16 half-cycles of 257. */
static void sr_run(via6522_t *v, uint32_t cycles) {
    int32_t t = (int32_t)v->sr_timer - (int32_t)cycles;
    while (t <= 0 && v->sr_halves) {
        v->cb1 = !v->cb1;
        sr_edge(v, v->cb1);
        t += sr_half_period(v);
    }
    v->sr_timer = (int16_t)(t > 0 ? t : sr_half_period(v));
}

/* ---- the control lines --------------------------------------------------- */

/* A read or write of port A: CA1's flag clears, CA2's unless it is an
 * independent input, and CA2 answers in handshake or pulse mode. */
static void port_a_access(via6522_t *v) {
    uint8_t mode = ca2_mode(v);
    v->ifr = (uint8_t)(v->ifr & ~VIA_INT_CA1);
    if (c2_cleared_by_port(mode)) v->ifr = (uint8_t)(v->ifr & ~VIA_INT_CA2);
    if (mode == C2_HANDSHAKE) v->ca2 = false;
    else if (mode == C2_PULSE) v->ca2_pulses++;
}

/* Port B the same, but only a write handshakes. */
static void port_b_access(via6522_t *v, bool write) {
    uint8_t mode = cb2_mode(v);
    v->ifr = (uint8_t)(v->ifr & ~VIA_INT_CB1);
    if (c2_cleared_by_port(mode)) v->ifr = (uint8_t)(v->ifr & ~VIA_INT_CB2);
    if (!write || sr_drives_cb2(via6522_sr_mode(v))) return;
    if (mode == C2_HANDSHAKE) v->cb2 = false;
    else if (mode == C2_PULSE) v->cb2_pulses++;
}

/* The PCR's output modes put their level out as soon as they are set;
 * a handshake or pulse output idles high. */
static void c2_outputs(via6522_t *v) {
    uint8_t a = ca2_mode(v), b = cb2_mode(v);
    if (a >= C2_HANDSHAKE) v->ca2 = a != C2_LOW;
    if (b >= C2_HANDSHAKE && !sr_drives_cb2(via6522_sr_mode(v))) v->cb2 = b != C2_LOW;
}

void via6522_set_ca1(via6522_t *v, bool level) {
    if (level == v->ca1) return;
    v->ca1 = level;
    if (level != ((v->pcr & 0x01u) != 0)) return;          /* not the active edge */
    if (v->acr & VIA_ACR_PA_LATCH) v->ira = port_in(v->ora, v->ddra, v->in_a);
    v->ifr |= VIA_INT_CA1;
    if (ca2_mode(v) == C2_HANDSHAKE) v->ca2 = true;
}

void via6522_set_ca2(via6522_t *v, bool level) {
    uint8_t mode = ca2_mode(v);
    if (mode >= C2_HANDSHAKE || level == v->ca2) return;
    v->ca2 = level;
    if (level == ((mode & 2u) != 0)) v->ifr |= VIA_INT_CA2;
}

void via6522_set_cb1(via6522_t *v, bool level) {
    uint8_t sr = via6522_sr_mode(v);
    if (sr_clocks_cb1(sr) || level == v->cb1) return;
    v->cb1 = level;
    if (v->sr_halves && sr != VIA_SR_OFF) sr_edge(v, level);
    if (level != ((v->pcr & 0x10u) != 0)) return;
    if (v->acr & VIA_ACR_PB_LATCH) v->irb = v->in_b;
    v->ifr |= VIA_INT_CB1;
    if (cb2_mode(v) == C2_HANDSHAKE && !sr_drives_cb2(sr)) v->cb2 = true;
}

void via6522_set_cb2(via6522_t *v, bool level) {
    uint8_t mode = cb2_mode(v), sr = via6522_sr_mode(v);
    if (sr_drives_cb2(sr) || level == v->cb2) return;
    v->cb2 = level;
    /* While the shift register takes its data off CB2, it is only data. */
    if (sr != VIA_SR_OFF || mode >= C2_HANDSHAKE) return;
    if (level == ((mode & 2u) != 0)) v->ifr |= VIA_INT_CB2;
}

void via6522_set_pa(via6522_t *v, uint8_t pins) {
    v->in_a = pins;
}

void via6522_set_pb(via6522_t *v, uint8_t pins) {
    bool fell = (v->in_b & 0x40u) && !(pins & 0x40u);
    v->in_b = pins;
    if (!fell || !(v->acr & VIA_ACR_T2_PULSES)) return;
    /* Counting pulses, T2 flags when it reaches zero, and counts on. */
    v->t2 = (v->t2 - 1) & 0xFFFF;
    if (v->t2 == 0 && v->t2_armed) {
        v->ifr |= VIA_INT_T2;
        v->t2_armed = false;
    }
}

/* ---- registers ------------------------------------------------------------ */

uint8_t ATOM_HOT1(via6522_read)(via6522_t *v, uint8_t reg) {
    switch (reg & 15u) {
    case VIA_ORB: {
        port_b_access(v, false);
        uint8_t pins = (v->acr & VIA_ACR_PB_LATCH) ? v->irb : v->in_b;
        uint8_t r = port_in(v->orb, v->ddrb, pins);
        if (v->acr & VIA_ACR_T1_PB7) r = (uint8_t)((r & 0x7Fu) | (v->pb7 ? 0x80u : 0u));
        return r;
    }
    case VIA_ORA:
        port_a_access(v);
        /* fall through */
    case VIA_ORA_NH:
        return (v->acr & VIA_ACR_PA_LATCH) ? v->ira : port_in(v->ora, v->ddra, v->in_a);
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
    case VIA_SR: {
        uint8_t r = v->sr;
        sr_start(v);
        return r;
    }
    case VIA_ACR:    return v->acr;
    case VIA_PCR:    return v->pcr;
    case VIA_IFR:    return (uint8_t)(v->ifr | (via6522_irq(v) ? VIA_INT_ANY : 0u));
    default:         return (uint8_t)(v->ier | 0x80u);   /* IER reads bit 7 set */
    }
}

void ATOM_HOT1(via6522_write)(via6522_t *v, uint8_t reg, uint8_t val) {
    switch (reg & 15u) {
    case VIA_ORB:    v->orb = val; port_b_access(v, true); break;
    case VIA_ORA:    v->ora = val; port_a_access(v); break;
    case VIA_ORA_NH: v->ora = val; break;
    case VIA_DDRB:   v->ddrb = val; break;
    case VIA_DDRA:   v->ddra = val; break;
    case VIA_T1CL:
    case VIA_T1LL:
        v->t1_latch = (uint16_t)((v->t1_latch & 0xFF00u) | val);
        break;
    case VIA_T1CH:
        /* Latch high, then the whole latch into the counter: this is
         * the write that starts T1, and PB7 goes low until it runs out. */
        v->t1_latch = (uint16_t)((v->t1_latch & 0x00FFu) | (val << 8));
        v->t1 = v->t1_latch;
        v->t1_armed = true;
        v->pb7 = false;
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
    case VIA_SR:     v->sr = val; sr_start(v); break;
    case VIA_ACR:
        v->acr = val;
        /* Turned off, the shift register stops where it is. */
        if (via6522_sr_mode(v) == VIA_SR_OFF) v->sr_halves = 0;
        c2_outputs(v);
        break;
    case VIA_PCR:    v->pcr = val; c2_outputs(v); break;
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

void ATOM_HOT1(via6522_tick)(via6522_t *v, uint32_t cycles) {
    v->t1 -= (int32_t)cycles;
    while (v->t1 < 0) {
        if (v->acr & VIA_ACR_T1_FREERUN) {
            /* The counter holds -1 for a cycle and reloads on the next,
             * so the period is latch + 2 (6522 data sheet, T1 free-run),
             * and PB7 is a square wave of twice that. */
            if (v->t1_armed) v->ifr |= VIA_INT_T1;
            v->pb7 = !v->pb7;
            v->t1 += (int32_t)v->t1_latch + 2;
        } else {
            if (v->t1_armed) { v->ifr |= VIA_INT_T1; v->pb7 = true; }
            v->t1_armed = false;
            v->t1 += 0x10000;      /* one-shot keeps counting, silently */
        }
    }

    /* Counting pulses on PB6 is via6522_set_pb's. */
    if (!(v->acr & VIA_ACR_T2_PULSES)) {
        v->t2 -= (int32_t)cycles;
        if (v->t2 < 0) {
            if (v->t2_armed) v->ifr |= VIA_INT_T2;
            v->t2_armed = false;
            v->t2 += 0x10000;
        }
    }

    if (__builtin_expect(v->sr_halves != 0, 0) && sr_clocks_cb1(via6522_sr_mode(v)))
        sr_run(v, cycles);
}
