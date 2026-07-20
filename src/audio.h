// Commander X16 Emulator
// Copyright (c) 2020 Frank van den Hoef
// All rights reserved. License: 2-clause BSD

#pragma once

#include <SDL.h>

#define AUDIO_SAMPLERATE (25000000 / 512)

void audio_init(const char *dev_name, int num_audio_buffers);
void audio_close(void);
void audio_step(int cpu_clocks);
void audio_render();

void audio_usage(void);

/* Control-API capture (retro-remote-debug-controller): tee the mixed output +
 * drain it for GET /audio. Both run on the emulator thread. */
void audio_capture_push(const int16_t *s, uint32_t n);
uint32_t audio_capture_drain(int16_t *out, uint32_t cap, uint32_t *dropped);
