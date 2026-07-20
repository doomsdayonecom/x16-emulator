/* control_backend.c — Commander X16 backend for the retro remote debug
 * controller (retro_control.h, from the retro-remote-debug-controller submodule
 * at extern/). Maps the portable read-only backend onto x16emu internals. */

#include "retro_control.h"

#include "glue.h"     /* struct regs regs; machine_reset() */
#include "memory.h"   /* debug_read6502, write6502, memory_get/set_ram_bank */
#include "video.h"    /* video_get_framebuffer, video_get_frame_count */
#include "keyboard.h" /* handle_keyboard (input injection) */
#include "audio.h"    /* audio_capture_drain, AUDIO_SAMPLERATE */

#include <SDL.h>
#include <stdbool.h>
#include <stdio.h>

/* Debug-read into out. bank<0 => current bank. $0000-$9FFF is low RAM
 * (bank-independent); $A000-$BFFF is banked RAM; $C000+ is ROM. */
static uint32_t
x16_read_mem(uint32_t addr, int32_t bank, uint32_t len, uint8_t *out, uint32_t cap)
{
	int16_t x16bank = (bank >= 0) ? (int16_t)bank : USE_CURRENT_X16_BANK;
	uint32_t n = 0;
	for (uint32_t i = 0; i < len && n < cap; i++) {
		uint16_t a = (uint16_t)(addr + i);
		out[n++] = debug_read6502(a, 0, x16bank);
	}
	return n;
}

/* 0.3: debug-write (poke). bank<0 => current. Skips ROM ($C000+). Uses the CPU
 * write path — intended for RAM/state; I/O ($9Fxx) writes trigger devices. */
static uint32_t
x16_write_mem(uint32_t addr, int32_t bank, uint32_t len, const uint8_t *in)
{
	uint8_t saved = memory_get_ram_bank();
	if (bank >= 0) {
		memory_set_ram_bank((uint8_t)bank);
	}
	uint32_t n = 0;
	for (; n < len; n++) {
		uint16_t a = (uint16_t)(addr + n);
		if (a >= 0xC000) {          /* ROM: not writable */
			break;
		}
		write6502(a, 0, in[n]);
	}
	memory_set_ram_bank(saved);
	return n;
}

static void
x16_get_regs_json(char *buf, size_t cap)
{
	snprintf(buf, cap,
		"{\"a\":%u,\"x\":%u,\"y\":%u,\"sp\":%u,\"pc\":%u,\"status\":%u,"
		"\"ram_bank\":%u,\"rom_bank\":%u}",
		(unsigned)regs.a, (unsigned)regs.xl, (unsigned)regs.yl,
		(unsigned)(regs.sp & 0xff), (unsigned)regs.pc, (unsigned)regs.status,
		(unsigned)memory_get_ram_bank(), (unsigned)memory_get_rom_bank());
}

static void
x16_get_framebuffer(retro_framebuffer_t *out)
{
	int w = 0, h = 0;
	out->pixels = video_get_framebuffer(&w, &h);
	out->width = w;
	out->height = h;
	out->fmt = RETRO_PIX_BGRA8888;   /* x16 framebuffer byte order */
}

static uint64_t
x16_get_frame_count(void)
{
	return video_get_frame_count();
}

/* Input injection (0.2). text => the char's key (letters use the lowercase
 * keycode); code => a raw SDL scancode. A tap presses then releases; hold with
 * down=1 across /step frames for a program that samples input each frame. */
static void
x16_key_event(SDL_Keycode sym, SDL_Scancode sc, int action)
{
	if (action == RETRO_KEY_DOWN) {
		handle_keyboard(true, sym, sc);
	} else if (action == RETRO_KEY_UP) {
		handle_keyboard(false, sym, sc);
	} else {                             /* RETRO_KEY_TAP */
		handle_keyboard(true, sym, sc);
		handle_keyboard(false, sym, sc);
	}
}

static int
x16_inject_key(int is_text, uint32_t value, int action)
{
	SDL_Keycode sym;
	SDL_Scancode sc;
	if (is_text) {
		int c = (int)value;
		if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';   /* keycodes are lowercase */
		sym = (SDL_Keycode)c;
		sc = SDL_GetScancodeFromKey(sym);
	} else {
		sc = (SDL_Scancode)value;                      /* raw SDL scancode */
		sym = SDL_GetKeyFromScancode(sc);
	}
	if (sc == SDL_SCANCODE_UNKNOWN) {
		return 0;
	}
	x16_key_event(sym, sc, action);
	return 1;
}

static void
x16_reset(void)
{
	machine_reset();
}

/* 0.3: drain the mixed audio the emulator has synthesised since the last call. */
static uint32_t
x16_capture_audio(int16_t *out, uint32_t cap, int *rate, int *channels, uint32_t *dropped)
{
	*rate = AUDIO_SAMPLERATE;
	*channels = 2;
	return audio_capture_drain(out, cap, dropped);
}

static const retro_control_backend_t x16_backend = {
	.platform        = "x16",
	.emulator        = "x16emu",
	.read_mem        = x16_read_mem,
	.get_regs_json   = x16_get_regs_json,
	.get_framebuffer = x16_get_framebuffer,
	.get_frame_count = x16_get_frame_count,
	.inject_key      = x16_inject_key,
	.reset           = x16_reset,
	.write_mem       = x16_write_mem,
	.capture_audio   = x16_capture_audio,
};

int
x16_control_start(int port)
{
	return retro_control_start(port, &x16_backend);
}
