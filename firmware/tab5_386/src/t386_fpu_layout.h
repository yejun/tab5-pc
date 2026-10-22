// Where translated code finds the x87's fields (tiny386's struct FPU is fpu.c's own): t386_fpu_jit.h fills it in,
// t386_jit.h emits code with it
#pragma once
struct T386FpuLayout { int cw, top, rawtagw, sv, sb; };
void t386_fpu_layout(struct T386FpuLayout *l);
