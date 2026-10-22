// Build settings for the tiny386 core on the Tab5. Every core file is compiled through a wrapper (t386_*.c) that
// includes this first, in place of upstream's `-include board_<name>.h`; the core itself stays unmodified in
// vendor/tiny386 (found through -I in tools/build.sh).
#pragma once
#define BUILD_ESP32
#define BPP 16                 // RGB565 frame buffer, which the PPA rotates and scales onto the panel
#define FULL_UPDATE            // redraw the whole text screen each refresh (no per-cell change tracking)
#define MIXER_BUF_LEN 512
#define I386_ENABLE_FPU        // compiled in; switched on per machine with [cpu] fpu = 1
#define USE_FMOPL              // AdLib
#define IRAM_ATTR_CPU_EXEC1    // the decoder loop is too big for IRAM (as on upstream's P4 board)
#include "driver/uart.h"       // misc.c's emulated serial port reads UART0 without including it
