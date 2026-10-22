// tiny386 core: fpu.c (see tab5_config.h)
#include "tab5_config.h"
#pragma GCC optimize("O3")
#include <stdbool.h>
struct FPU;
static bool t386_fparith_fast(struct FPU *fpu, int group, unsigned d, double a, double b);   // (t386_fpu_jit.h)
#define T386_FPARITH_FAST t386_fparith_fast
static double t386_single(unsigned bits);   // registers kept as singles (vendor/patches/0013, t386_fpu_jit.h)
#define T386_FPU_SINGLES t386_single
#include "fpu.c"
#include "t386_fpu_jit.h"   // (the frequent forms for translated code)
