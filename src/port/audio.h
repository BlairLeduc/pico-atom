/* audio.h — PWM slice, chained DMA, and the PCM queue (design.md §9.4).
 *
 * Core 0 owns all of it: the field loop pushes the guest's samples in
 * thread context and DMA_IRQ_0, also on core 0, drains them into the
 * ring. The queue is single-producer single-consumer between those two.
 *
 * Pushing blocks while the queue is full, and that is the emulator's
 * throttle (§12.2): the PWM wrap is a hardware clock derived from
 * clk_sys, and pacing on it holds the queue at depth without a
 * wall-clock timer.
 */
#ifndef PICO_ATOM_AUDIO_H
#define PICO_ATOM_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Claim the slice for GP26/GP27 and two DMA channels, and start the ring
 * playing silence. Call once, after board_init_clocks and after anything
 * that claims fixed DMA channels (the LCD claims channel 0). The carrier
 * is computed from clock_get_hz(clk_sys), never assumed; clk_sys is set
 * once at boot, so there is no retune path yet — a clock change needs a
 * stop, retune and restart (hardware-notes.md §3, §5.2). */
void audio_init(void);

/* The sample rate as the exact fraction rate_num / rate_den Hz, for
 * atom_audio_set_rate (design.md §9.2). */
void audio_rate(uint32_t *rate_num, uint32_t *rate_den);

/* Queue signed 16-bit mono, blocking while the queue is full. Playback
 * starts once ATOM_PCM_QUEUE_START samples are waiting. */
void audio_push(const int16_t *pcm, size_t n);

/* Samples the queue can take now without blocking. Turbo (main.c)
 * tops the queue up with silence to this, so an unpaced guest never
 * waits on it and the ring never runs dry. */
size_t audio_room(void);

/* 0..256. Muted samples are still queued and consumed, so muting does
 * not change the guest's timing (hardware-notes.md §5.8). */
void audio_set_volume(unsigned volume);
void audio_set_muted(bool muted);

typedef struct {
    uint32_t underrun_samples;  /* the queue was empty when DMA wanted one */
    uint32_t late_refills;      /* the IRQ found its half already replaying */
    uint32_t consumed;          /* samples taken off the queue, or padded   */
    uint32_t level;             /* queue depth now                          */
    uint32_t low_water;         /* lowest depth since the last reset        */
    bool     started;
} audio_stats_t;

/* Two counters, not one (hardware-notes.md §5.8): an underrun is the
 * producer's failure, a late refill the consumer's. */
void audio_stats(audio_stats_t *st, bool reset_low_water);

#endif /* PICO_ATOM_AUDIO_H */
