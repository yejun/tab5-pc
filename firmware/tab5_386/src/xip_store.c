// Where the emulator's code is kept in flash (see xip.h): filled in after the link by tools/xip_patch.py. In a file
// of its own so that the compiler can't take its contents for the zeros they are here.
#include "xip.h"
const unsigned char t386_xip_store[T386_XIP_MAX] = {0};
