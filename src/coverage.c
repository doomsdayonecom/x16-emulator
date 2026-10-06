// Commander X16 Emulator
// Execution coverage: which addresses the CPU fetched an instruction from.
// See coverage.h for the file it writes.

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "memory.h"
#include "coverage.h"

bool coverage_enabled = false;
volatile bool coverage_stop = false;

static const char *coverage_path;   // argv's, which lives as long as the program
static uint8_t *low;      // $0000-$9FFF
static uint8_t *ram;      // 256 banks of $A000-$BFFF
static uint8_t *rom;      // 256 banks of $C000-$FFFF

#define LOW_SIZE  0xa000
#define RAM_SIZE  0x2000
#define ROM_SIZE  0x4000

// SIGTERM ends the CPU loop rather than the process, so main's shutdown runs
// and the marks are written; a second SIGTERM, or SIGKILL, still ends it.
static void
coverage_sigterm(int sig)
{
	(void)sig;
	coverage_stop = true;
	signal(SIGTERM, SIG_DFL);
}

bool
coverage_init(const char *path)
{
	low = calloc(LOW_SIZE, 1);
	ram = calloc(256 * RAM_SIZE, 1);
	rom = calloc(256 * ROM_SIZE, 1);
	coverage_path = path;
	if (!low || !ram || !rom) {
		return false;
	}
	coverage_enabled = true;
	signal(SIGTERM, coverage_sigterm);
	return true;
}

void
coverage_mark(uint16_t pc)
{
	if (pc < 0xa000) {
		low[pc] = 1;
	} else if (pc < 0xc000) {
		ram[(memory_get_ram_bank() << 13) + (pc - 0xa000)] = 1;
	} else {
		rom[(memory_get_rom_bank() << 14) + (pc - 0xc000)] = 1;
	}
}

// One line per run of marked addresses in [0, size), named from base.
static void
write_runs(FILE *f, char kind, int bank, const uint8_t *marks, int size, int base)
{
	for (int i = 0; i < size; i++) {
		if (!marks[i]) {
			continue;
		}
		int start = i;
		while (i + 1 < size && marks[i + 1]) {
			i++;
		}
		if (bank < 0) {
			fprintf(f, "%c %04x-%04x\n", kind, base + start, base + i);
		} else {
			fprintf(f, "%c %02x:%04x-%04x\n", kind, bank, base + start, base + i);
		}
	}
}

void
coverage_write(void)
{
	if (!coverage_enabled) {
		return;
	}
	FILE *f = fopen(coverage_path, "w");
	if (!f) {
		fprintf(stderr, "Cannot write coverage to %s!\n", coverage_path);
		return;
	}
	fprintf(f, "x16emu-coverage 1\n");
	write_runs(f, 'm', -1, low, LOW_SIZE, 0);
	for (int bank = 0; bank < 256; bank++) {
		write_runs(f, 'r', bank, ram + (bank << 13), RAM_SIZE, 0xa000);
	}
	for (int bank = 0; bank < 256; bank++) {
		write_runs(f, 'k', bank, rom + (bank << 14), ROM_SIZE, 0xc000);
	}
	fclose(f);
}
