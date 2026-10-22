// tiny386 core: i8254.c (see tab5_config.h), with abort() on unsupported timer modes replaced (t386_pit_fix.h)
#include "tab5_config.h"
#pragma GCC optimize("O3")
#include <stdlib.h>
#include "t386_pit_fix.h"
#include "i8254.c"
#undef abort
