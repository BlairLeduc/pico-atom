/* audio.c — PWM slice, chained DMA, and the PCM queue (design.md §9.4).
 *
 * Straight from hardware-notes.md §5.3-5.4: two DMA channels chained to
 * each other ping-pong a two-half ring, paced by the slice's wrap DREQ,
 * each writing the 32-bit left | right << 16 compare word. The ring is
 * power-of-two sized and aligned with the hardware read wrap, so a
 * refill held off by a flash erase replays the ring rather than playing
 * whatever follows it in SRAM (§7.2).
 */

#include "audio.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"

#include "config.h"

#define AUDIO_PIN_L 26u   /* PWM5 A, hardware-notes.md §1.1 */
#define AUDIO_PIN_R 27u   /* PWM5 B */

#define PWM_DIV     1u
#define PWM_MID     ((ATOM_PWM_TOP + 1u) / 2u)

#define RING_BYTES  (ATOM_DMA_RING_SLOTS * sizeof(uint32_t))
#define RING_BITS   11u
_Static_assert(RING_BYTES == (1u << RING_BITS), "the ring must be 2^RING_BITS bytes");

/* Power of two, so the indices can run free and wrap by mask. */
_Static_assert((ATOM_PCM_QUEUE_LEN & (ATOM_PCM_QUEUE_LEN - 1u)) == 0,
               "the PCM queue must be a power of two");

/* Aligned to its own size: channel_config_set_ring wraps the low
 * RING_BITS of the read address, so anything less and the wrap lands
 * outside the buffer. */
static uint32_t s_ring[ATOM_DMA_RING_SLOTS] __attribute__((aligned(RING_BYTES)));

/* Compare words, ready to copy. Written by thread context, read by the
 * IRQ, both on core 0. */
static uint32_t s_queue[ATOM_PCM_QUEUE_LEN];
static volatile uint32_t s_head;          /* thread only */
static volatile uint32_t s_tail;          /* IRQ only    */
static volatile bool     s_started;

static volatile uint32_t s_underruns, s_late, s_consumed, s_low_water;

static int s_ch[2];
static uint32_t s_rate_num, s_rate_den;
static unsigned s_volume = 256u;
static bool s_muted;

#define SILENCE (PWM_MID | (PWM_MID << 16))

/* One half, from the queue. After an underrun the queue simply carries
 * on from wherever the producer has got to — the missing samples are
 * counted, not replayed (design.md §12.2). */
static void __not_in_flash_func(fill_half)(uint32_t *dst) {
    uint32_t tail = s_tail;
    uint32_t head = s_head;
    uint32_t under = 0;
    for (unsigned i = 0; i < ATOM_DMA_FRAMES_PER_HALF; i++) {
        uint32_t w = SILENCE;
        if (s_started) {
            if (tail != head) w = s_queue[tail++ & (ATOM_PCM_QUEUE_LEN - 1u)];
            else under++;
        }
        /* Oversample 2: each frame occupies two slots (§9.4). */
        dst[2u * i] = w;
        dst[2u * i + 1u] = w;
    }
    __compiler_memory_barrier();
    s_tail = tail;
    if (s_started) {
        s_consumed += ATOM_DMA_FRAMES_PER_HALF;
        s_underruns += under;
        uint32_t level = head - tail;
        if (level < s_low_water) s_low_water = level;
    }
}

static void __not_in_flash_func(refill)(unsigned which) {
    int ch = s_ch[which];
    uint32_t *half = &s_ring[which * ATOM_DMA_SLOTS_PER_HALF];

    /* This channel finished and chained to the other. If it is running
     * again already, the other half has drained too and chained back
     * here before this IRQ was serviced: the deadline was missed. The
     * channel is live — un-re-armed, it is replaying the other half, and
     * the ring wrap keeps it inside the buffer — so it is left alone
     * until its next completion re-arms it. Nothing is taken off the
     * queue either: that completion refills this half, so samples put
     * here now would be overwritten before they played. Left queued,
     * they are only late. */
    if (dma_channel_is_busy(ch)) {
        s_late++;
        return;
    }
    fill_half(half);
    /* A chain trigger reloads neither the address nor the count: set
     * both, or the next chain completes at once and storms the IRQ
     * (hardware-notes.md §5.3). */
    dma_channel_set_read_addr(ch, half, false);
    dma_channel_set_trans_count(ch, ATOM_DMA_SLOTS_PER_HALF, false);
}

static void __not_in_flash_func(audio_irq)(void) {
    for (unsigned which = 0; which < 2u; which++) {
        uint32_t bit = 1u << s_ch[which];
        if (dma_hw->ints0 & bit) {
            dma_hw->ints0 = bit;
            refill(which);
        }
    }
}

void audio_init(void) {
    gpio_set_function(AUDIO_PIN_L, GPIO_FUNC_PWM);
    gpio_set_function(AUDIO_PIN_R, GPIO_FUNC_PWM);
    /* Both pins are one slice — ask, do not hard-code (§5.1). */
    unsigned slice = pwm_gpio_to_slice_num(AUDIO_PIN_L);

    pwm_config pc = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&pc, PWM_DIV);
    pwm_config_set_wrap(&pc, ATOM_PWM_TOP);
    pwm_init(slice, &pc, false);
    pwm_set_both_levels(slice, PWM_MID, PWM_MID);

    /* The cadence as a fraction, never the truncated 36,621 (§9.2). */
    s_rate_num = clock_get_hz(clk_sys);
    s_rate_den = PWM_DIV * (ATOM_PWM_TOP + 1u) * ATOM_PWM_OVERSAMPLE;

    for (unsigned i = 0; i < ATOM_DMA_RING_SLOTS; i++) s_ring[i] = SILENCE;
    s_head = s_tail = 0;
    s_started = false;
    s_low_water = ATOM_PCM_QUEUE_LEN;

    s_ch[0] = dma_claim_unused_channel(true);
    s_ch[1] = dma_claim_unused_channel(true);
    for (unsigned which = 0; which < 2u; which++) {
        dma_channel_config c = dma_channel_get_default_config(s_ch[which]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, pwm_get_dreq(slice));
        channel_config_set_ring(&c, false, RING_BITS);
        channel_config_set_chain_to(&c, s_ch[which ^ 1u]);
        dma_channel_configure(s_ch[which], &c,
                              &pwm_hw->slice[slice].cc,
                              &s_ring[which * ATOM_DMA_SLOTS_PER_HALF],
                              ATOM_DMA_SLOTS_PER_HALF, false);
        dma_channel_set_irq0_enabled(s_ch[which], true);
    }

    /* Above the default 0x80, so a refill preempts a 4-5 ms southbridge
     * transaction or anything else at default priority (§5.4). */
    irq_set_exclusive_handler(DMA_IRQ_0, audio_irq);
    irq_set_priority(DMA_IRQ_0, 0x40);
    irq_set_enabled(DMA_IRQ_0, true);

    pwm_set_enabled(slice, true);
    dma_channel_start(s_ch[0]);
}

void audio_rate(uint32_t *rate_num, uint32_t *rate_den) {
    *rate_num = s_rate_num;
    *rate_den = s_rate_den;
}

void audio_set_volume(unsigned volume) {
    s_volume = volume > 256u ? 256u : volume;
}

void audio_set_muted(bool muted) {
    s_muted = muted;
}

static inline uint32_t compare_word(int16_t s) {
    if (s_muted) return SILENCE;
    /* ±32767 at full volume is ±1023 about the mid-point: 11 bits. */
    int32_t v = (int32_t)PWM_MID + ((int32_t)s * (int32_t)s_volume) / (1 << 13);
    if (v < 0) v = 0;
    if (v > (int32_t)ATOM_PWM_TOP) v = ATOM_PWM_TOP;
    /* Mono, duplicated to both ears (§9.4). */
    return (uint32_t)v | ((uint32_t)v << 16);
}

void audio_push(const int16_t *pcm, size_t n) {
    uint32_t head = s_head;
    for (size_t i = 0; i < n; i++) {
        /* Full: wait for the next refill. This is the throttle (§12.2).
         * The DMA IRQ is on this core, so it is also what wakes us. */
        while (head - s_tail >= ATOM_PCM_QUEUE_LEN) __wfi();
        s_queue[head & (ATOM_PCM_QUEUE_LEN - 1u)] = compare_word(pcm[i]);
        __compiler_memory_barrier();
        s_head = ++head;
        if (!s_started && head - s_tail >= ATOM_PCM_QUEUE_START) s_started = true;
    }
}

size_t audio_room(void) {
    return ATOM_PCM_QUEUE_LEN - (size_t)(s_head - s_tail);
}

void audio_stats(audio_stats_t *st, bool reset_low_water) {
    st->underrun_samples = s_underruns;
    st->late_refills = s_late;
    st->consumed = s_consumed;
    st->level = s_head - s_tail;
    st->low_water = s_low_water;
    st->started = s_started;
    if (reset_low_water) s_low_water = st->level;
}
