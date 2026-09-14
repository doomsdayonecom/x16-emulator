// Commander X16 Emulator — a virtual serial/network card.
//
// A 16450-compatible UART at an expansion base with a Zimodem-enough modem
// behind it: Hayes AT commands in command mode, and a dial that opens a real
// TCP socket from the emulator process, after which bytes flow both ways.
// Enough for a program written against the TexElec serial/ESP32 card to be
// tested here, dialling a service on the developer's machine, without the
// card. It is not a model of the card's timing; a byte written is on the
// wire at once and a byte from the wire is readable at once.
//
// Only the card's network half (the eight registers at the base) exists.
// The DE-9 serial half, eight bytes above it, reads as open bus.

#pragma once

#include <stdbool.h>
#include <stdint.h>

extern bool has_netcard;
extern uint16_t netcard_addr;

void netcard_init(void);
void netcard_step(void);
uint8_t netcard_read(uint8_t reg, bool debugOn);
void netcard_write(uint8_t reg, uint8_t val);
