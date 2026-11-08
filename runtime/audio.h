/* Audio output: WASAPI shared mode on its own thread, fed through a lock-free ring buffer.
 * The emulator pushes native-rate SNES samples; they are resampled to the device rate with
 * dynamic rate control (tiny pitch adjustments, inaudible) so the buffer never runs dry or
 * overflows, whatever the display refresh / frame pacing is. */
#pragma once
#include <stdint.h>

int  audio_init(void);                              /* 0 if no audio device (game runs silent) */
void audio_shutdown(void);
void audio_push(const int16_t *stereo, int frames, double in_rate, int volume);
void audio_clear(void);                             /* drop queued audio (pause, reset, fast-forward) */
double audio_latency_ms(void);
void audio_stats(long *underruns, long *fill_min, long *fill_max, long *callbacks);   /* since last call */
