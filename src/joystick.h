#pragma once
#ifndef JOYSTICK_H
#define JOYSTICK_H

#include <stdint.h>
#include <stdbool.h>

#define JOY_LATCH_MASK 0x04
#define JOY_CLK_MASK 0x08

#define NUM_JOYSTICKS 4

extern uint8_t Joystick_data;
extern bool Joystick_slots_enabled[NUM_JOYSTICKS];

bool joystick_init(void); //initialize SDL controllers
void joystick_add(int index);
void joystick_remove(int index);

void joystick_button_down(int instance_id, uint8_t button);
void joystick_button_up(int instance_id, uint8_t button);

void joystick_set_latch(bool value);
void joystick_set_clock(bool value);

/* --- Virtual pads, for the RRDC control port (contract 0.5) ----------
 *
 * A harness has no SDL controller to plug in, so it asks for one to be
 * synthesised in a joystick slot. `mask` is the X16's own SNES bit
 * order and is ACTIVE LOW, exactly like joystick_info::button_mask —
 * converting from RRDC's canonical layout is the caller's job, not
 * this layer's.
 *
 * A virtual pad is a LEVEL, not an event: what it holds persists across
 * frames until set again, which is what makes "hold RIGHT for 60
 * frames" expressible. Returns false if `slot` is out of range. */
bool joystick_virtual_set(int slot, bool connected, bool set_buttons, uint16_t mask);

/* Read back what is present in `slot` — a virtual pad OR a real one, so
 * a harness can tell "no controller at all" from "controller present,
 * nothing held". Returns false if `slot` is out of range. */
bool joystick_virtual_get(int slot, bool *connected, uint16_t *mask);

#endif
