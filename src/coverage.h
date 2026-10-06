// Commander X16 Emulator
// Execution coverage: which addresses the CPU fetched an instruction from.

/*
-coverage <file> marks every address the CPU runs an instruction from, keeping
the bank for the two banked windows, and writes the marks to <file> when the
emulator exits: normally, or on SIGTERM, which is how a test harness stops it.
A host tool maps them to source lines through the program's line tables.

The file is text, one run of consecutive marked addresses a line:

    x16emu-coverage 1
    m 0801-0843          low memory, $0000-$9FFF
    r 03:a000-a01f       a RAM bank in the $A000-$BFFF window
    k 20:c000-c00a       a ROM or cartridge bank in the $C000-$FFFF window

An address is marked once however often it runs: one byte of memory per
address, and no counting in the CPU loop.
*/

#ifndef COVERAGE_H
#define COVERAGE_H

#include <stdbool.h>
#include <stdint.h>

extern bool coverage_enabled;
extern volatile bool coverage_stop;

bool coverage_init(const char *path);
void coverage_mark(uint16_t pc);
void coverage_write(void);

#endif
