/* control_backend.c — Commander X16 backend for the retro remote debug
 * controller (retro_control.h, from the retro-remote-debug-controller submodule
 * at extern/). Maps the portable read-only backend onto x16emu internals. */

#include "retro_control.h"

#include "glue.h"     /* struct regs regs; machine_reset() */
#include "memory.h"   /* debug_read6502, write6502, memory_get_ram_bank/rom_bank */
#include "video.h"    /* video_get_framebuffer, video_get_frame_count, video_space_read */
#include "keyboard.h" /* handle_keyboard (input injection) */
#include "i2c.h"      /* mouse_move / mouse_button_* / mouse_send_state (pointer) */
#include "audio.h"    /* AUDIO_SAMPLERATE */

#include <SDL.h>
#include <stdbool.h>
#include <stdio.h>

extern uint32_t host_sample_rate;   /* audio.c: the real (post-SDL) output rate */

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

/* Write (poke) len bytes at addr (0.3), through the same bus a CPU write uses.
 * Low RAM ($0000-$9EFF) is flat; banked RAM ($A000-$BFFF) hits the current RAM
 * bank on gen1 (write6502 forces bank 0 there) or the flat bank on gen2. */
static uint32_t
x16_write_mem(uint32_t addr, int32_t bank, uint32_t len, const uint8_t *in)
{
	uint8_t x16bank = (bank >= 0) ? (uint8_t)bank : 0;
	for (uint32_t i = 0; i < len; i++) {
		write6502((uint16_t)(addr + i), x16bank, in[i]);
	}
	return len;
}

/* --- Audio drain (0.3): a lock-free ring teed from the mixer. The producer
 * (audio_render, via x16_control_push_audio) and the consumer (capture_audio)
 * both run on the emulator thread, so no locking is needed. --- */
#define X16_AUD_RING 65536              /* int16 samples (~0.67 s stereo @ 48.8 kHz) */
static int16_t  g_aud[X16_AUD_RING];
static uint32_t g_aud_head, g_aud_count, g_aud_dropped;
static int      g_aud_active;           /* start capturing only once the server is up */

/* Called from audio_render() for every mixed stereo pair. */
void
x16_control_push_audio(int16_t l, int16_t r)
{
	if (!g_aud_active) return;
	if (g_aud_count + 2 > X16_AUD_RING) { g_aud_dropped += 2; return; }
	g_aud[(g_aud_head + g_aud_count) % X16_AUD_RING] = l; g_aud_count++;
	g_aud[(g_aud_head + g_aud_count) % X16_AUD_RING] = r; g_aud_count++;
}

static uint32_t
x16_capture_audio(int16_t *out, uint32_t cap, int *rate, int *channels, uint32_t *dropped)
{
	uint32_t n = 0;
	if (rate)     *rate     = host_sample_rate ? (int)host_sample_rate : (int)AUDIO_SAMPLERATE;
	if (channels) *channels = 2;
	if (dropped)  *dropped  = g_aud_dropped;
	g_aud_dropped = 0;
	while (n < cap && g_aud_count) {
		out[n++] = g_aud[g_aud_head];
		g_aud_head = (g_aud_head + 1) % X16_AUD_RING;
		g_aud_count--;
	}
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

/* Pointer injection (0.4). The X16 mouse is a relative PS/2 device: the KERNAL
 * integrates the deltas into an absolute position and writes it to VERA sprite 0
 * (the pointer). So we inject through the same mouse_move/mouse_send_state path a
 * host mouse uses (device level, not driver state), and read the position back
 * out of sprite 0's attributes. Absolute is closed-loop: read sprite 0, inject
 * the delta to the target. The move lands once the KERNAL's next VSYNC consumes
 * the packet, so a /step must follow before it is visible — and the guest must
 * have the mouse enabled (KERNAL MOUSE / VERA sprite 0) for it to mean anything. */
#define VERA_SPRITE0_ATTR 0x1FC00U   /* sprite 0 attribute base in VERA VRAM */

static int32_t
x16_pointer_x(void)
{
	return (int32_t)video_space_read(VERA_SPRITE0_ATTR + 2)
	     | ((int32_t)(video_space_read(VERA_SPRITE0_ATTR + 3) & 3) << 8);
}
static int32_t
x16_pointer_y(void)
{
	return (int32_t)video_space_read(VERA_SPRITE0_ATTR + 4)
	     | ((int32_t)(video_space_read(VERA_SPRITE0_ATTR + 5) & 3) << 8);
}

static int
x16_set_pointer(int absolute, int32_t x, int32_t y, int buttons)
{
	if (absolute) {
		mouse_move((int)(x - x16_pointer_x()), (int)(y - x16_pointer_y()));
	} else {
		mouse_move((int)x, (int)y);            /* relative deltas, screen pixels */
	}
	if (buttons >= 0) {                        /* -1 => leave held buttons as-is */
		if (buttons & 1) mouse_button_down(0); else mouse_button_up(0); /* primary */
		if (buttons & 2) mouse_button_down(1); else mouse_button_up(1); /* secondary */
	}
	mouse_send_state();                        /* enqueue the PS/2 packet(s) */
	return 1;                                  /* the mouse device is always present */
}

static int
x16_get_pointer(int32_t *x, int32_t *y, int *buttons)
{
	*x = x16_pointer_x();
	*y = x16_pointer_y();
	*buttons = (int)(mouse_get_buttons() & 3);  /* bit0 primary, bit1 secondary */
	return 1;
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
	.set_pointer     = x16_set_pointer,
	.get_pointer     = x16_get_pointer,
};

int
x16_control_start(int port)
{
	g_aud_active = 1;                 /* begin teeing mixer output into the ring */
	return retro_control_start(port, &x16_backend);
}
