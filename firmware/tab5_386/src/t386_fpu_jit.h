// The x87 for translated code: single precision on the host's FPU, and the x87's frequent memory forms for
// translated code (t386_jit.h, K_FPU), included after tiny386's fpu.c: the translated code loads the operand itself
// (its fast memory path, faults before anything here changes) or stores what these return (then FSTP's pop: a store
// that faults is done again from the start). As fpu_exec2 does them, without its decoding and its memory accesses
// through the segments and page tables. Built on tiny386's fpu.c (Copyright (c) 2024-2025 Chunhui He, BSD-3-Clause:
// LICENSES/BSD-3-Clause-tiny386.txt).
#include <stddef.h>
#include <stdint.h>
#include "t386_fpu_layout.h"

// counts of the quick ways taken and why not (T386_FPU_STATS builds: serial "fpu", t386_jit.h's t386_fpu_report)
enum {
	FS_A32_HIT, FS_A32_OPND, FS_A32_ST, FS_A32_PC, FS_A32_RES,   // FADD.. m32: done; the operand, ST(0), PC, the result
	FS_R_HIT, FS_R_ST0, FS_R_STI, FS_R_PC, FS_R_RES,             // FADD.. ST(i): the same
	FS_LD32_HIT, FS_LD32_SLOW, FS_ST32_HIT, FS_ST32_SLOW, FS_FIST_HIT, FS_FIST_SLOW, FS_MOVE_HIT, FS_MOVE_SLOW,
	FS_FPARITH, FS_FPARITH_HOOK,                                 // fparith's calls, those its hook did
	// why the inline code (t386_jit.h) called a helper whose quick way then did it: a register not kept as a single,
	// a zero operand, a zero result, else (the control word not the translation's)
	FS_IN_KEPT, FS_IN_ZOP, FS_IN_ZRES, FS_IN_CW,
	FS_N
};
#ifdef T386_FPU_STATS
uint32_t t386_fpu_stat[FS_N];
#define T386_FS(i) (t386_fpu_stat[i]++)
#else
#define T386_FS(i) ((void) 0)
#endif

// ---- single precision on the host's FPU (RISC-V F: the P4's) where it gives the x87's result: precision control
// single, both operands singles exactly, the result a normal single (the x87's registers keep the exponent's range
// beyond a single's: overflow, underflow, a NaN or a division by zero go the long way). Doubles and singles are
// converted by their bits (the P4 has no double FPU: the C conversions are libgcc's, in software).

// a double that is exactly a normal single (or a zero): its bits
static inline bool t386_d2f(double d, uint32_t *f)
{
	union union64 u = { .f = d };
	const uint32_t sign = (uint32_t) (u.i >> 32) & 0x80000000;
	if ((u.i << 1) == 0) { *f = sign; return true; }
	const int e = (int) ((u.i >> 52) & 0x7ff) - 1023;
	if ((u.i & ((1ull << 29) - 1)) || e < -126 || e > 127) return false;
	*f = sign | (uint32_t) (e + 127) << 23 | (uint32_t) (u.i >> 29) & 0x7fffff;
	return true;
}

// a normal single's bits as a double
static inline double t386_f32n(uint32_t bits)
{
	union union64 u;
	u.i = (uint64_t) (bits & 0x80000000) << 32 | (uint64_t) (((bits >> 23) & 0xff) - 127 + 1023) << 52 |
	      (uint64_t) (bits & 0x7fffff) << 29;
	return u.f;
}
static inline bool t386_normal_f32(uint32_t bits) { return ((bits >> 23) & 0xff) - 1 < 254; }
// a normal single or a zero: as a double by its bits
static inline bool t386_nz_f32(uint32_t bits) { return t386_normal_f32(bits) || !(bits << 1); }
static inline double t386_f32nz(uint32_t bits)
{
	if (bits << 1) return t386_f32n(bits);
	union union64 u = { .i = (uint64_t) (bits & 0x80000000) << 32 };
	return u.f;
}

// an integer as a double, by its bits (libgcc's __floatsidf otherwise)
static inline double t386_i2d(int32_t v)
{
	union union64 u = { .i = 0 };
	if (v) {
		const uint32_t m = v < 0 ? -(uint32_t) v : (uint32_t) v;
		const int lz = __builtin_clz(m);   // (the top bit's at 31 - lz: the exponent)
		u.i = (uint64_t) ((v < 0 ? 0x80000000u : 0) | (uint32_t) (31 - lz + 1023) << 20) << 32 |
		      ((uint64_t) m << (lz + 21) & 0x000fffffffffffffull);
	}
	return u.f;
}

// FCOM's C3 C2 C0 for a (ST(0)) against b as fparith sets them, by the doubles' bits (fparith's compares are libgcc's)
static inline void t386_fcom(FPU *fpu, double a, double b)
{
	union union64 x = { .f = a }, y = { .f = b };
	const uint64_t inf = 0xffe0000000000000ull;   // (an infinity, the sign shifted out: beyond it, a NaN)
	unsigned c;
	if ((x.i << 1) > inf || (y.i << 1) > inf) {
		c = 0x4500;   // C3 C2 C0: unordered
	} else if (x.i == y.i || ((x.i | y.i) << 1) == 0) {
		c = 0x4000;   // C3: equal (+0 = -0)
	} else {   // sign and magnitude as an unsigned order
		const uint64_t kx = x.i >> 63 ? ~x.i : x.i | 1ull << 63, ky = y.i >> 63 ? ~y.i : y.i | 1ull << 63;
		c = kx < ky ? 0x100 : 0;   // C0: less
	}
	fpu->sw = (fpu->sw & ~0x4500) | c;
}

// a denormal, an infinity or a NaN (libgcc's), out of line: the quick ways' callers then keep no registers
static __attribute__((noinline)) double t386_f32_other(uint32_t bits) { union union32 v = { .i = bits }; return v.f; }

// a single's bits as a double (normal or zero by the bits, the rest by libgcc)
static inline double t386_f32(uint32_t bits)
{
	if (t386_normal_f32(bits)) return t386_f32n(bits);
	if (bits << 1) return t386_f32_other(bits);
	union union64 u = { .i = (uint64_t) (bits & 0x80000000) << 32 };   // a zero
	return u.f;
}

// The registers the quick ways write are kept as singles (vendor/patches/0013: sb[i] when sv's bit i is set, st[i]
// made by fpget when tiny386's code wants it): no conversion to a double and back for each instruction.
static double t386_single(unsigned bits) { return t386_f32nz(bits); }

// st[idx] (an x87 register, not a stack position) as a single's bits, when it is exactly a normal single or a zero:
// kept as one, or st[idx] up to date (fpget's test: else it is in rawst), which is then kept as a single too (for
// the inline code: a value FLD1 or FILD pushed, say)
static inline bool t386_st_f32(FPU *fpu, unsigned idx, uint32_t *f)
{
	if (fpu->sv >> idx & 1) {
		*f = fpu->sb[idx];
		return true;
	}
	if (!((fpu->rawtagr | fpu->rawtagw) >> idx & 1) || !t386_d2f(fpu->st[idx], f)) return false;
	fpu->sb[idx] = *f;
	fpu->sv |= 1 << idx;   // (st[idx] the same value: its tags as they are)
	return true;
}
static inline void t386_st_set_f32(FPU *fpu, unsigned idx, uint32_t bits)   // (fpset's, as a single)
{
	fpu->sb[idx] = bits;
	fpu->sv |= 1 << idx;
	fpu->rawtagw |= 1 << idx;
}

#if defined(__riscv) && defined(__riscv_flen) && __riscv_flen >= 32
#define T386_F32(op, rm) asm volatile(op ".s %0, %1, %2, " rm : "=f"(r) : "f"(x), "f"(y))
// a op b (0 add, 1 mul, 4 sub, 6 div) in rounding mode rc as a single; false: maybe not the x87's. A result with
// its exponent well inside a single's range is the x87's; the rest (zeros, near or beyond the smallest and largest
// normal singles, where the rounding of a single and of the x87's registers part, infinities, NaNs) go the long
// way. (Checked by the exponent: reading the FPU's exception flags took more time than the operation.)
static inline bool t386_hw_f32(int op, uint32_t a, uint32_t b, int rc, uint32_t *out)
{
	float x, y, r;
	memcpy(&x, &a, 4);
	memcpy(&y, &b, 4);
	switch (op * 4 + rc) {
	case 0: T386_F32("fadd", "rne"); break;
	case 1: T386_F32("fadd", "rdn"); break;
	case 2: T386_F32("fadd", "rup"); break;
	case 3: T386_F32("fadd", "rtz"); break;
	case 4: T386_F32("fmul", "rne"); break;
	case 5: T386_F32("fmul", "rdn"); break;
	case 6: T386_F32("fmul", "rup"); break;
	case 7: T386_F32("fmul", "rtz"); break;
	case 16: T386_F32("fsub", "rne"); break;
	case 17: T386_F32("fsub", "rdn"); break;
	case 18: T386_F32("fsub", "rup"); break;
	case 19: T386_F32("fsub", "rtz"); break;
	case 24: T386_F32("fdiv", "rne"); break;
	case 25: T386_F32("fdiv", "rdn"); break;
	case 26: T386_F32("fdiv", "rup"); break;
	case 27: T386_F32("fdiv", "rtz"); break;
	default: return false;
	}
	memcpy(out, &r, 4);
	const uint32_t e = (*out >> 23) & 0xff;
	if (e >= 2 && e <= 253) return true;
	// a zero, when it is exact: a sum's or a difference's (denormals aren't flushed: a nonzero one shows as one), a
	// product's or a quotient's of a zero; not one by underflow, which the x87's range keeps
	return !(*out << 1) && (op == 0 || op == 4 || !(a << 1) || !(b << 1));
}

#define T386_FCVT(rm) asm volatile("fcvt.w.s %0, %1, " rm : "=r"(r) : "f"(x))
// a single (bits) to an int32 in rounding mode rc; false: maybe out of range (|a| from 2^30 on, or a NaN)
static inline bool t386_hw_f32_to_i32(uint32_t a, int rc, int32_t *out)
{
	float x;
	int32_t r;
	if (((a >> 23) & 0xff) >= 127 + 30) return false;
	memcpy(&x, &a, 4);
	switch (rc) {
	case 0: T386_FCVT("rne"); break;
	case 1: T386_FCVT("rdn"); break;
	case 2: T386_FCVT("rup"); break;
	default: T386_FCVT("rtz"); break;
	}
	*out = r;
	return true;
}
#define T386_HW_F32 1

// fparith's group (FADD FMUL - - FSUB FSUBR FDIV FDIVR) of the singles a (ST(0)) and b in single precision, as the
// x87 with the precision control single gives it: r, a normal single; false: the long way
static inline bool t386_hw_group(const FPU *fpu, int group, uint32_t a, uint32_t b, uint32_t *r)
{
	static const signed char op[8] = { 0, 1, -1, -1, 4, 4, 6, 6 };
	const bool rev = group == 5 || group == 7;   // FSUBR, FDIVR: b op a
	return (fpu->cw & 0x300) == 0 && t386_hw_f32(op[group], rev ? b : a, rev ? a : b, (fpu->cw >> 10) & 3, r);
}

// fparith's hook (vendor/patches/0012): FADD FMUL FSUB FSUBR FDIV FDIVR in single precision; FCOM FCOMP by the bits
static bool t386_fparith_fast(struct FPU *fpu, int group, unsigned d, double a, double b)
{
	uint32_t fa, fb, r;
	T386_FS(FS_FPARITH);
	if (group == 2 || group == 3) {
		t386_fcom(fpu, a, b);
		if (group == 3) fppop(fpu);
		T386_FS(FS_FPARITH_HOOK);
		return true;
	}
	if ((fpu->cw & 0x300) != 0) return false;
	if (!t386_d2f(a, &fa) || !t386_d2f(b, &fb) || !t386_hw_group(fpu, group, fa, fb, &r)) return false;
	T386_FS(FS_FPARITH_HOOK);
	t386_st_set_f32(fpu, (fpu->top + d) & 7, r);
	return true;
}
#else
static bool t386_fparith_fast(struct FPU *fpu, int group, unsigned d, double a, double b)
{
	T386_FS(FS_FPARITH);
	if (group == 2 || group == 3) {
		t386_fcom(fpu, a, b);
		if (group == 3) fppop(fpu);
		T386_FS(FS_FPARITH_HOOK);
		return true;
	}
	return false;
}
#endif

// ---- the frequent memory forms
static inline double t386_f64(uint32_t lo, uint32_t hi) { union union64 u = { .i = (uint64_t) hi << 32 | lo }; return u.f; }

// D8 /g m32, DC /g m64, DA /g m32int, DE /g m16int: FADD FMUL FCOM FCOMP FSUB FSUBR FDIV FDIVR, ST(0) with the operand
void t386_fpu_arith(FPU *fpu, int g, double b)
{
	fparith(fpu, g, 0, fpget(fpu, 0), b);
}
void t386_fpu_arith_f32(FPU *fpu, int g, uint32_t bits) { t386_fpu_arith(fpu, g, t386_f32(bits)); }

// D8 /g m32 but FCOM(P), one helper for each group (the translator knows it: the operation and the operands' order
// constants here): fparith's hook's single precision straight from the operand's bits first; the rest out of line,
// as is everything that would keep registers
static __attribute__((noinline)) void t386_fpu_arith_f32_other(FPU *fpu, int g, uint32_t bits)
{
	t386_fpu_arith(fpu, g, t386_f32(bits));
}
static inline __attribute__((always_inline)) void t386_arith_f32(FPU *fpu, int g, uint32_t bits)
{
#ifdef T386_HW_F32
	const unsigned top = fpu->top & 7;
	uint32_t a, r;
#ifdef T386_FPU_STATS
	const unsigned kept = fpu->sv >> top & 1;
#endif
	if (t386_nz_f32(bits) && t386_st_f32(fpu, top, &a) && t386_hw_group(fpu, g, a, bits, &r)) {
		T386_FS(FS_A32_HIT);
		T386_FS(!kept ? FS_IN_KEPT : !(bits << 1) || !(a << 1) ? FS_IN_ZOP : !(r << 1) ? FS_IN_ZRES : FS_IN_CW);
		t386_st_set_f32(fpu, top, r);
		return;
	}
#ifdef T386_FPU_STATS
	T386_FS(!t386_nz_f32(bits) ? FS_A32_OPND : !t386_st_f32(fpu, top, &a) ? FS_A32_ST :
		(fpu->cw & 0x300) ? FS_A32_PC : FS_A32_RES);
#endif
#endif
	t386_fpu_arith_f32_other(fpu, g, bits);
}
void t386_fpu_fadd_f32(FPU *fpu, uint32_t bits) { t386_arith_f32(fpu, 0, bits); }
void t386_fpu_fmul_f32(FPU *fpu, uint32_t bits) { t386_arith_f32(fpu, 1, bits); }
void t386_fpu_fsub_f32(FPU *fpu, uint32_t bits) { t386_arith_f32(fpu, 4, bits); }
void t386_fpu_fsubr_f32(FPU *fpu, uint32_t bits) { t386_arith_f32(fpu, 5, bits); }
void t386_fpu_fdiv_f32(FPU *fpu, uint32_t bits) { t386_arith_f32(fpu, 6, bits); }
void t386_fpu_fdivr_f32(FPU *fpu, uint32_t bits) { t386_arith_f32(fpu, 7, bits); }
void t386_fpu_arith_f64(FPU *fpu, int g, uint32_t lo, uint32_t hi) { t386_fpu_arith(fpu, g, t386_f64(lo, hi)); }
void t386_fpu_arith_i32(FPU *fpu, int g, uint32_t v) { t386_fpu_arith(fpu, g, t386_i2d((int32_t) v)); }
void t386_fpu_arith_i16(FPU *fpu, int g, uint32_t v) { t386_fpu_arith(fpu, g, t386_i2d((int16_t) v)); }

// D9 /0 FLD m32, DD /0 FLD m64, DB /0 FILD m32, DF /0 FILD m16
static __attribute__((noinline)) void t386_fpu_ld_f32_other(FPU *fpu, uint32_t bits) { fppush(fpu, t386_f32(bits)); }
void t386_fpu_ld_f32(FPU *fpu, uint32_t bits)
{
	if (t386_nz_f32(bits)) {
		T386_FS(FS_LD32_HIT);
		fpu->top = (fpu->top - 1) & 7;
		t386_st_set_f32(fpu, fpu->top, bits);
		return;
	}
	T386_FS(FS_LD32_SLOW);
	t386_fpu_ld_f32_other(fpu, bits);
}
void t386_fpu_ld_f64(FPU *fpu, uint32_t lo, uint32_t hi) { fppush(fpu, t386_f64(lo, hi)); }
// (FILD: a single when it is one exactly, |v| up to 2^24: kept as one for the inline code)
static inline void t386_fpu_ld_int(FPU *fpu, int32_t v)
{
#ifdef T386_HW_F32
	if ((uint32_t) v + 0x1000000 <= 0x2000000) {
		const float f = (float) v;   // (fcvt.s.w: exact)
		uint32_t bits;
		memcpy(&bits, &f, 4);
		fpu->top = (fpu->top - 1) & 7;
		t386_st_set_f32(fpu, fpu->top, bits);
		return;
	}
#endif
	fppush(fpu, t386_i2d(v));
}
void t386_fpu_ld_i32(FPU *fpu, uint32_t v) { t386_fpu_ld_int(fpu, (int32_t) v); }
void t386_fpu_ld_i16(FPU *fpu, uint32_t v) { t386_fpu_ld_int(fpu, (int16_t) v); }

// D9 /2 /3 FST(P) m32, DD /2 /3 FST(P) m64: ST(0) as stored (m32: rounded in the rounding mode)
static __attribute__((noinline)) uint32_t t386_fpu_st_f32_other(FPU *fpu)
{
	const double a = fpget(fpu, 0);
	uint32_t bits;
	if (t386_d2f(a, &bits)) return bits;
	union union32 u = { .f = f32_round(a, (fpu->cw >> 10) & 3) };
	return u.i;
}
uint32_t t386_fpu_st_f32(FPU *fpu)
{
	uint32_t bits;
	if (t386_st_f32(fpu, fpu->top & 7, &bits)) {
		T386_FS(FS_ST32_HIT);
		return bits;
	}
	T386_FS(FS_ST32_SLOW);
	return t386_fpu_st_f32_other(fpu);
}
uint64_t t386_fpu_st_f64(FPU *fpu)
{
	union union64 u = { .f = fpget(fpu, 0) };
	return u.i;
}

// DB /2 /3 FIST(P) m32, DF /2 /3 FIST(P) m16: ST(0) rounded to an integer in the rounding mode (out of range: the
// integer indefinite); a single by fcvt.w.s first
static __attribute__((noinline)) uint32_t t386_fpu_st_i32_other(FPU *fpu)
{
	double a = fpround(fpget(fpu, 0), (fpu->cw >> 10) & 3);
	return a < 2147483648.0 && a >= -2147483648.0 ? (uint32_t) (int32_t) a : 0x80000000;
}
uint32_t t386_fpu_st_i32(FPU *fpu)
{
#ifdef T386_HW_F32
	uint32_t bits;
	int32_t v;
	if (t386_st_f32(fpu, fpu->top & 7, &bits) && t386_hw_f32_to_i32(bits, (fpu->cw >> 10) & 3, &v)) {
		T386_FS(FS_FIST_HIT);
		return v;
	}
#endif
	T386_FS(FS_FIST_SLOW);
	return t386_fpu_st_i32_other(fpu);
}
static __attribute__((noinline)) uint32_t t386_fpu_st_i16_other(FPU *fpu)
{
	double a = fpround(fpget(fpu, 0), (fpu->cw >> 10) & 3);
	return a < 32768.0 && a >= -32768.0 ? (uint16_t) (int16_t) a : 0x8000;
}
uint32_t t386_fpu_st_i16(FPU *fpu)
{
#ifdef T386_HW_F32
	uint32_t bits;
	int32_t v;
	if (t386_st_f32(fpu, fpu->top & 7, &bits) && t386_hw_f32_to_i32(bits, (fpu->cw >> 10) & 3, &v) &&
	    v == (int16_t) v)
		return (uint16_t) v;
#endif
	return t386_fpu_st_i16_other(fpu);
}

// D9 /5 FLDCW, D9 /7 FNSTCW
void t386_fpu_set_cw(FPU *fpu, uint32_t cw) { fpu->cw = cw; }
uint32_t t386_fpu_get_cw(FPU *fpu) { return fpu->cw; }

// register forms that can't fault or need the CPU (t386_jit.h's jt_fpu_reg_form): fpu_exec1 straight away
void t386_fpu_reg(FPU *fpu, uint32_t op_group, uint32_t i) { fpu_exec1(fpu, NULL, op_group >> 3, op_group & 7, i); }

// D8 /g ST(0), ST(i), DC /g ST(i), ST(0), DE /g ...P (g: FADD FMUL FSUB FSUBR FDIV FDIVR), a helper for each group as
// for m32: fparith's hook's single precision straight from the registers first
static inline __attribute__((always_inline)) void t386_reg_arith(FPU *fpu, int g, uint32_t op_group, uint32_t i)
{
#ifdef T386_HW_F32
	const unsigned esc = op_group >> 3, top = fpu->top & 7, sti = (top + i) & 7;
	uint32_t a, b, r;
#ifdef T386_FPU_STATS
	const unsigned kept = (fpu->sv >> top & 1) && (fpu->sv >> sti & 1);
#endif
	if (t386_st_f32(fpu, top, &a) && t386_st_f32(fpu, sti, &b) && t386_hw_group(fpu, g, a, b, &r)) {
		T386_FS(FS_R_HIT);
		T386_FS(!kept ? FS_IN_KEPT : !(a << 1) || !(b << 1) ? FS_IN_ZOP : !(r << 1) ? FS_IN_ZRES : FS_IN_CW);
		t386_st_set_f32(fpu, esc ? sti : top, r);
		if (esc == 6) fpu->top = (top + 1) & 7;
		return;
	}
#ifdef T386_FPU_STATS
	T386_FS(!t386_st_f32(fpu, top, &a) ? FS_R_ST0 : !t386_st_f32(fpu, sti, &b) ? FS_R_STI :
		(fpu->cw & 0x300) ? FS_R_PC : FS_R_RES);
#endif
#endif
	fpu_exec1(fpu, NULL, op_group >> 3, g, i);
}
void t386_fpu_fadd_reg(FPU *fpu, uint32_t op_group, uint32_t i) { t386_reg_arith(fpu, 0, op_group, i); }
void t386_fpu_fmul_reg(FPU *fpu, uint32_t op_group, uint32_t i) { t386_reg_arith(fpu, 1, op_group, i); }
void t386_fpu_fsub_reg(FPU *fpu, uint32_t op_group, uint32_t i) { t386_reg_arith(fpu, 4, op_group, i); }
void t386_fpu_fsubr_reg(FPU *fpu, uint32_t op_group, uint32_t i) { t386_reg_arith(fpu, 5, op_group, i); }
void t386_fpu_fdiv_reg(FPU *fpu, uint32_t op_group, uint32_t i) { t386_reg_arith(fpu, 6, op_group, i); }
void t386_fpu_fdivr_reg(FPU *fpu, uint32_t op_group, uint32_t i) { t386_reg_arith(fpu, 7, op_group, i); }

// a register's value into another, as fpset does it: the single kept and st[]'s double both (one of them stale), and
// whether it is kept as a single
static inline void t386_st_copy(FPU *fpu, unsigned to, unsigned from)
{
	const unsigned sv = fpu->sv;
	fpu->sb[to] = fpu->sb[from];
	fpu->st[to] = fpu->st[from];
	fpu->sv = (sv & ~(1u << to)) | (sv >> from & 1) << to;
	fpu->rawtagw |= 1 << to;
}
static inline bool t386_st_valid(const FPU *fpu, unsigned idx)   // up to date: kept as a single or in st[]
{
	return (fpu->rawtagr | fpu->rawtagw) >> idx & 1;   // (kept as a single: rawtagw too)
}

// D9 /0 FLD ST(i), D9 /1 FXCH, DD /2 FST ST(i), DD /3 FSTP ST(i), a helper each: the registers' values moved as they
// are kept, when they are up to date (else in rawst: fpu_exec1)
static __attribute__((noinline)) void t386_fpu_move_other(FPU *fpu, uint32_t op_group, uint32_t i)
{
	T386_FS(FS_MOVE_SLOW);
	fpu_exec1(fpu, NULL, op_group >> 3, op_group & 7, i);
}
void t386_fpu_fld_reg(FPU *fpu, uint32_t op_group, uint32_t i)
{
	const unsigned top = fpu->top & 7, sti = (top + i) & 7;
	if (!t386_st_valid(fpu, sti)) {
		t386_fpu_move_other(fpu, op_group, i);
		return;
	}
	T386_FS(FS_MOVE_HIT);
	fpu->top = (top - 1) & 7;
	t386_st_copy(fpu, (top - 1) & 7, sti);
}
void t386_fpu_fxch_reg(FPU *fpu, uint32_t op_group, uint32_t i)
{
	const unsigned top = fpu->top & 7, sti = (top + i) & 7;
	if (!t386_st_valid(fpu, sti) || !t386_st_valid(fpu, top)) {
		t386_fpu_move_other(fpu, op_group, i);
		return;
	}
	T386_FS(FS_MOVE_HIT);
	const uint32_t b = fpu->sb[sti];
	const double d = fpu->st[sti];
	const unsigned sv = fpu->sv, v = sv >> sti & 1;
	t386_st_copy(fpu, sti, top);
	fpu->sb[top] = b;
	fpu->st[top] = d;
	fpu->sv = (fpu->sv & ~(1u << top)) | v << top;
	fpu->rawtagw |= 1 << top;
}
void t386_fpu_fst_reg(FPU *fpu, uint32_t op_group, uint32_t i)
{
	const unsigned top = fpu->top & 7, sti = (top + i) & 7;
	if (!t386_st_valid(fpu, top)) {
		t386_fpu_move_other(fpu, op_group, i);
		return;
	}
	T386_FS(FS_MOVE_HIT);
	t386_st_copy(fpu, sti, top);
}
void t386_fpu_fstp_reg(FPU *fpu, uint32_t op_group, uint32_t i)
{
	const unsigned top = fpu->top & 7, sti = (top + i) & 7;
	if (!t386_st_valid(fpu, top)) {
		t386_fpu_move_other(fpu, op_group, i);
		return;
	}
	T386_FS(FS_MOVE_HIT);
	t386_st_copy(fpu, sti, top);
	fpu->top = (top + 1) & 7;
}

// DF E0 FNSTSW AX: the status word, for AX
uint32_t t386_fpu_get_sw(FPU *fpu) { return getsw(fpu); }

// for checking the translator: the FPU's size (it copies the state out and back, no pointers
// in it), and two states the same? (by their fields: F80's padding is whatever was on the stack)
size_t t386_fpu_state_size(void) { return sizeof(FPU); }
// the fields translated code uses (t386_fpu_layout.h: the registers kept as singles, inline)
void t386_fpu_layout(struct T386FpuLayout *l)
{
	l->cw = offsetof(FPU, cw);
	l->top = offsetof(FPU, top);
	l->rawtagw = offsetof(FPU, rawtagw);
	l->sv = offsetof(FPU, sv);
	l->sb = offsetof(FPU, sb);
}
// (the registers by their values: a single kept or st[]'s double, which st[] may not have yet)
bool t386_fpu_same(const FPU *a, const FPU *b)
{
	if (a->cw != b->cw || a->sw != b->sw || a->top != b->top || a->rawtagr != b->rawtagr || a->rawtagw != b->rawtagw)
		return false;
	for (int i = 0; i < 8; i++) {
		union union64 x = { .f = a->sv >> i & 1 ? t386_single(a->sb[i]) : a->st[i] },
			      y = { .f = b->sv >> i & 1 ? t386_single(b->sb[i]) : b->st[i] };
		if (((a->rawtagr | a->rawtagw) >> i & 1) && x.i != y.i) return false;
		if (a->rawst[i].mant0 != b->rawst[i].mant0 || a->rawst[i].mant1 != b->rawst[i].mant1 ||
		    a->rawst[i].high != b->rawst[i].high)
			return false;
	}
	return true;
}
