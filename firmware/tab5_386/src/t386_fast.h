// A fast path for tiny386's CPU. Included at the end of a translation unit that includes tiny386's i386.c (the
// firmware's t386_i386.c), with cpui386_step renamed to tiny386_step before that include:
// this file defines the cpui386_step that pc.c calls.
//
// tiny386 decodes every instruction through general helpers: segment and paging checks, an MMIO range check and an
// OptAddr record per memory access, prefix and operand-size branches, one shared indirect jump. On the Tab5 that is
// ~100 RISC-V instructions per x86 instruction. Here the common instructions are decoded inline, memory operands
// become host pointers through a small TLB of our own (linear page -> host address, separately for reads and
// writes), instruction bytes are read through a host pointer, and every handler dispatches the next instruction
// itself.
//
// It works on tiny386's CPU state as it is, including its lazy flags, and computes results with tiny386's own
// instruction macros (ADDw, SHLd, DIVw, ...), so the two can take turns at any instruction boundary: whatever isn't
// handled here (system and protected-mode control instructions, segment loads in protected mode, I/O privilege
// checks, the FPU, MMIO such as VGA memory, page-crossing accesses, faults, virtual-8086 mode) goes to tiny386's
// cpu_exec1 for that one instruction. A handler hands over before it changes anything, so tiny386 starts the
// instruction afresh; that's also how faults are raised.
//
// Built on tiny386's internal definitions, and cpui386_step below is adapted from tiny386's: those parts are
// Copyright (c) 2024-2025 Chunhui He, BSD-3-Clause (LICENSES/BSD-3-Clause-tiny386.txt).

#ifndef T386_FAST_IRAM
#define T386_FAST_IRAM
#endif

typedef u16 __attribute__((may_alias)) u16a;
typedef u32 __attribute__((may_alias)) u32a;

// Translated code (t386_jit.h): a byte per 4 KB page of the physical address space, nonzero bits 0-1 where writes
// must be checked (the page holds translated code); NULL without the translator.
u8 *t386_pagemap;
void t386_code_write(uword addr, int len);

// ---- the TLB: linear page -> host address of RAM, filled from tiny386's page translation
// Hardware interrupts taken and INT instructions run, by vector (serial "cpu"); and with T386_INT_STATS the INTs
// the fast path runs, by vector and AH.
unsigned t386_int_hw[256], t386_int_vec[256];
#ifdef T386_INT_STATS
unsigned t386_int_sw[256][256];
#endif

static u32 fast_slow_ops[528];   // instructions handed to tiny386, by opcode (0F xx at 256 + xx; 0F 00 /r and 0F 01 /r
				 // at 512 + 8 * xx + r): serial "fast ops"
void t386_fast_ops_report(char *buf, int len)
{
	int n = 0;
	for (int k = 0; k < 16 && n < len - 40; k++) {
		int best = -1;
		for (int i = 0; i < 528; i++)
			if (fast_slow_ops[i] && (best < 0 || fast_slow_ops[i] > fast_slow_ops[best])) best = i;
		if (best < 0) break;
		if (best >= 512)
			n += snprintf(buf + n, len - n, "0f %02x /%d: %u\n", (best - 512) >> 3, best & 7, fast_slow_ops[best]);
		else
			n += snprintf(buf + n, len - n, "%s%02x: %u\n", best >= 256 ? "0f " : "", best & 255, fast_slow_ops[best]);
		fast_slow_ops[best] = 0;
	}
	memset(fast_slow_ops, 0, sizeof(fast_slow_ops));
}

#define FTLB_N 1024
struct ftlb_entry {
	uword rtag, wtag;   // the linear page number this entry maps for reads / for writes, or -1
	uintptr_t delta;    // host address = linear address + delta
	uword ctag;         // the page for writes that go through t386_code_write (translated code there), or -1
				// (16 bytes: indexing is a shift)
};
// Two, as pages are user or supervisor ones: for privilege level 3, and for 0-2 (and without paging). A change of
// privilege level picks the other (a protected-mode GUI may change it thousands of times a second); a change of CR0
// or CR3 empties both.
static struct ftlb_entry ftlb_both[2][FTLB_N];
static struct ftlb_entry *ftlb = ftlb_both[0];   // the one in use (translated code loads it: jit_translate)
static uword ftlb_cr0 = -1, ftlb_cr3;            // what the entries were made under (CR0: FTLB_CR0 bits)
// Without paging the entries are linear = physical, in real and protected mode alike (DOS extenders switch
// between them for each DOS call): only paging and write protection empty them
#ifndef FTLB_CR0
#define FTLB_CR0 (CR0_PG | CR0_WP)
#endif

static u32 ftlb_flushes;   // (serial "jit")
static unsigned ftlb_gen;  // t386_tlb_gen when it was last emptied
static void (*ftlb_on_flush)(void);   // the translator's, for what it keeps by linear address

static inline struct ftlb_entry *ftlb_for(CPUI386 *cpu)
{
	return ftlb_both[(cpu->cr0 & CR0_PG) && cpu->cpl == 3];
}

static void ftlb_flush(CPUI386 *cpu)
{
	ftlb_flushes++;
	for (int k = 0; k < 2; k++)
		for (int i = 0; i < FTLB_N; i++)
			ftlb_both[k][i].rtag = ftlb_both[k][i].wtag = ftlb_both[k][i].ctag = -1;
	ftlb_cr0 = cpu->cr0 & FTLB_CR0;
	ftlb_cr3 = cpu->cr3;
	ftlb_gen = t386_tlb_gen;
	ftlb = ftlb_for(cpu);
	if (ftlb_on_flush) ftlb_on_flush();
}

// Entries depend on paging (CR0, CR3) and, with paging, on whether the privilege level is 3; and they go when
// tiny386's TLB does (MOV CR0/CR3, INVLPG, task switches: a task switch reloads CR3 even with the same value, the
// 386's way to flush the TLB after page tables change, CWSDPMI's).
static inline void ftlb_check(CPUI386 *cpu)
{
	if (unlikely((cpu->cr0 & FTLB_CR0) != ftlb_cr0 || ((cpu->cr0 & CR0_PG) && cpu->cr3 != ftlb_cr3) ||
		     t386_tlb_gen != ftlb_gen))
		ftlb_flush(cpu);
	else
		ftlb = ftlb_for(cpu);
}

// The host address of a size-byte access at linear address lin, filling the TLB; NULL if tiny386 must do the
// access: it is not to RAM (VGA memory and other MMIO), crosses a page, or faults (tiny386 raises the fault when
// it redoes the instruction).
static noinline u8 *ftlb_miss(CPUI386 *cpu, uword lin, int size, int write)
{
	if ((lin & 0xfff) > 0x1000 - size)
		return NULL;
	uword pg = lin >> 12;
	struct ftlb_entry *e = &ftlb[pg & (FTLB_N - 1)];
	if (write && e->ctag == pg && e->rtag == pg) {   // a page with translated code, written before
		u8 *host = (u8 *) (uintptr_t) (lin + e->delta);
		uword paddr = host - cpu->phys_mem;
		if (t386_pagemap[paddr >> 12] & 3)
			t386_code_write(paddr, size);
		else   // (its code has gone)
			e->wtag = pg, e->ctag = -1;
		return host;
	}
	uword paddr;
	if (cpu->cr0 & CR0_PG) {
		if (!translate_lpgno(cpu, write ? 2 : 1, pg, lin, cpu->cpl, &paddr))
			return NULL;
		paddr &= ~0xfff;
	} else {
		paddr = pg << 12;
	}
	if (in_iomem(paddr) || paddr >= (uword) cpu->phys_mem_size)
		return NULL;
	uintptr_t delta = (uintptr_t) cpu->phys_mem + paddr - (pg << 12);
	if (e->delta != delta || e->rtag != pg)
		e->wtag = e->ctag = -1;
	e->delta = delta;
	e->rtag = pg;
	if (write && t386_pagemap && (t386_pagemap[paddr >> 12] & 3)) {
		// translated code there: no write entry (each write comes here), and the blocks a write overlaps go first
		// (some programs have code, data and stacks in the same pages)
		e->ctag = pg;
		t386_code_write(paddr + (lin & 0xfff), size);
	} else if (write) {
		e->wtag = pg;
	}
	return (u8 *) (uintptr_t) (lin + delta);
}

// MMIO (VGA memory) through tiny386's callbacks: the physical address of a size-byte access at linear address lin,
// if it reaches MMIO without a fault and within a page; else -1 (tiny386 does the instruction).
static noinline uword fast_mmio_addr(CPUI386 *cpu, uword lin, int size, int write)
{
	if ((lin & 0xfff) > 0x1000 - size || !cpu->cb.iomem_read8)
		return -1;
	uword paddr = lin;
	if ((cpu->cr0 & CR0_PG) && !translate_lpgno(cpu, write ? 2 : 1, lin >> 12, lin, cpu->cpl, &paddr))
		return -1;
	return in_iomem(paddr) ? paddr : (uword) -1;
}

// (the VGA window first, through t386_vga_ext.h's fast paths where they apply)
int t386_vga_write_fast(uint32_t off, int size, uint32_t v);
int t386_vga_read_fast(uint32_t off, int size, uint32_t *out);

static inline u32 fast_io_read(CPUI386 *cpu, uword paddr, int size)
{
	u32 v;
	if (paddr - 0xa0000 < 0x10000 && t386_vga_read_fast(paddr - 0xa0000, size, &v)) return v;
	return size == 1 ? cpu->cb.iomem_read8(cpu->cb.iomem, paddr) :
	       size == 2 ? cpu->cb.iomem_read16(cpu->cb.iomem, paddr) : cpu->cb.iomem_read32(cpu->cb.iomem, paddr);
}

static inline void fast_io_write(CPUI386 *cpu, uword paddr, int size, u32 v)
{
	if (paddr - 0xa0000 < 0x10000 && t386_vga_write_fast(paddr - 0xa0000, size, v)) return;
	if (size == 1) cpu->cb.iomem_write8(cpu->cb.iomem, paddr, v);
	else if (size == 2) cpu->cb.iomem_write16(cpu->cb.iomem, paddr, v);
	else cpu->cb.iomem_write32(cpu->cb.iomem, paddr, v);
}

// Where a size-byte access at lin goes: 1 RAM (*host), 2 MMIO (*paddr), 0 through tiny386.
static noinline int fast_where(CPUI386 *cpu, uword lin, int size, int write, u8 **host, uword *paddr)
{
	struct ftlb_entry *e = &ftlb[(lin >> 12) & (FTLB_N - 1)];
	if ((write ? e->wtag : e->rtag) == lin >> 12 && (lin & 0xfff) <= (uword) (0x1000 - size)) {
		*host = (u8 *) (uintptr_t) (lin + e->delta);
		return 1;
	}
	if ((*host = ftlb_miss(cpu, lin, size, write)))
		return 1;
	*paddr = fast_mmio_addr(cpu, lin, size, write);
	return *paddr != (uword) -1 ? 2 : 0;
}

// Segments that would fault as null selectors in protected mode (tiny386's segcheck): accesses through them go to
// tiny386.
static inline unsigned fast_nullsegs(CPUI386 *cpu)
{
	unsigned m = 0;
	if (cpu->cr0 & 1) {
		for (int i = 0; i < 6; i++)
			if (cpu->seg[i].limit == 0 && (cpu->seg[i].sel & ~0x3) == 0)
				m |= 1 << i;
	}
	return m;
}

// ---- operands
// byte registers AL CL DL BL AH CH DH BH as offsets into gprx
static const u8 DRAM_ATTR fast_r8off[8] = { 0, 4, 8, 12, 1, 5, 9, 13 };
#define fr8(i) (((u8 *) cpu->gprx)[fast_r8off[i]])
#define fw8(i, v) (((u8 *) cpu->gprx)[fast_r8off[i]] = (v))
// memory operands through host pointers (the host is little-endian and handles unaligned accesses, as tiny386's
// I386_OPT1 already assumes)
#define fm8(p) (*(u8 *) (p))
#define fm16(p) (*(u16a *) (p))
#define fm32(p) (*(u32a *) (p))
#define fs8(p, v) (*(u8 *) (p) = (v))
#define fs16(p, v) (*(u16a *) (p) = (v))
#define fs32(p, v) (*(u32a *) (p) = (v))

// tiny386's CMP and TEST also try to skip a following JZ/JNZ through its fetch state, which isn't used here
#define FCMPb(...) AOP0_helper(SUB,  8, -, __VA_ARGS__)
#define FCMPw(...) AOP0_helper(SUB, 16, -, __VA_ARGS__)
#define FCMPd(...) AOP0_helper(SUB, 32, -, __VA_ARGS__)
#define FTESTb(...) LOP0_helper(AND,  8, &, __VA_ARGS__)
#define FTESTw(...) LOP0_helper(AND, 16, &, __VA_ARGS__)
#define FTESTd(...) LOP0_helper(AND, 32, &, __VA_ARGS__)

// The offset of a memory operand (modrm's mod != 3), reading its SIB byte and displacement; the segment is set to
// the default unless overridden.
static const u8 DRAM_ATTR fast_ea16_base[8] = { 3, 3, 5, 5, 6, 7, 5, 3 };
static const u8 DRAM_ATTR fast_ea16_index[8] = { 6, 7, 6, 7, 0, 0, 0, 0 };
static inline __attribute__((always_inline)) uword fast_ea(CPUI386 *cpu, const u8 **ppc, u8 modrm, int adsz16, int *seg)
{
	const u8 *pc = *ppc;
	int mod = modrm >> 6, rm = modrm & 7;
	uword a;
	if (adsz16) {
		if (mod == 0 && rm == 6) {
			a = *(u16a *) pc;
			pc += 2;
			if (*seg < 0)
				*seg = SEG_DS;
		} else {
			a = cpu->gprx[fast_ea16_base[rm]].r16 + (cpu->gprx[fast_ea16_index[rm]].r16 & -(uword) (rm < 4));
			if (mod == 1)
				a += (s8) *pc++;
			else if (mod == 2) {
				a += *(u16a *) pc;
				pc += 2;
			}
			a &= 0xffff;
			if (*seg < 0)
				*seg = (rm == 2 || rm == 3 || rm == 6) ? SEG_SS : SEG_DS;
		}
	} else {
		int dseg = SEG_DS;
		if (rm == 4) {
			u8 sib = *pc++;
			int b = sib & 7, i = (sib >> 3) & 7;
			if (b == 5 && mod == 0) {
				a = *(u32a *) pc;
				pc += 4;
			} else {
				a = REGi(b);
				if (b == 4 || b == 5)
					dseg = SEG_SS;
			}
			if (i != 4)
				a += REGi(i) << (sib >> 6);
		} else if (rm == 5 && mod == 0) {
			a = *(u32a *) pc;
			pc += 4;
		} else {
			a = REGi(rm);
			if (rm == 5)
				dseg = SEG_SS;
		}
		if (mod == 1)
			a += (s8) *pc++;
		else if (mod == 2) {
			a += *(u32a *) pc;
			pc += 4;
		}
		if (*seg < 0)
			*seg = dseg;
	}
	*ppc = pc;
	return a;
}

// tiny386's get_CF for the usual last operations, inline (ADC, SBB, RCL, RCR and the jumps on CF take it; tiny386's
// is a call through a switch), the same expressions; the rest from tiny386's
static inline __attribute__((always_inline)) int fast_get_CF(CPUI386 *cpu)
{
	if (!(cpu->cc.mask & CF))
		return !!(cpu->flags & CF);
	switch (cpu->cc.op) {
	case CC_ADC: return cpu->cc.dst <= cpu->cc.src2;
	case CC_ADD: return cpu->cc.dst < cpu->cc.src2;
	case CC_SBB: return cpu->cc.src1 <= cpu->cc.src2;
	case CC_SUB: return cpu->cc.src1 < cpu->cc.src2;
	case CC_SHL: case CC_SHR: case CC_SAR: return cpu->cc.dst2 & 1;
	case CC_AND: case CC_OR: case CC_XOR: return 0;
	}
	return (get_CF)(cpu);
}
#define get_CF(cpu) fast_get_CF(cpu)

// Conditions of Jcc/SETcc (tiny386's COND()), comparing the operands directly after a CMP or SUB.
static noinline int fast_cond_slow(CPUI386 *cpu, int c)
{
	int cond;
	switch (c >> 1) {
	case 0: cond = get_OF(cpu); break;
	case 1: cond = get_CF(cpu); break;
	case 2: cond = get_ZF(cpu); break;
	case 3: cond = get_ZF(cpu) || get_CF(cpu); break;
	case 4: cond = get_SF(cpu); break;
	case 5: cond = get_PF(cpu); break;
	case 6: cond = get_SF(cpu) != get_OF(cpu); break;
	default: cond = get_ZF(cpu) || get_SF(cpu) != get_OF(cpu); break;
	}
	return cond ^ (c & 1);
}

static inline __attribute__((always_inline)) int fast_cond(CPUI386 *cpu, int c)
{
	if ((c >> 1) == 2)
		return get_ZF(cpu) ^ (c & 1);
	if (cpu->cc.op == CC_SUB && cpu->cc.mask == (CF | PF | AF | ZF | SF | OF)) {
		// the operands are kept sign-extended, which keeps their order both signed and unsigned
		uword a = cpu->cc.src1, b = cpu->cc.src2;
		int cond;
		switch (c >> 1) {
		case 1: cond = a < b; break;
		case 3: cond = a <= b; break;
		case 4: cond = (sword) cpu->cc.dst < 0; break;
		case 6: cond = (sword) a < (sword) b; break;
		case 7: cond = (sword) a <= (sword) b; break;
		default: return fast_cond_slow(cpu, c);
		}
		return cond ^ (c & 1);
	}
	return fast_cond_slow(cpu, c);
}

// ---- the code window: instructions are read through a host pointer while they lie in plain RAM
// Finds the RAM around CS:eip. Unpaged, that's the whole RAM block containing it (below 640 KB, or from the BIOS
// ROM up); paged, its page. The window ends 15 bytes early so that an instruction started in it lies wholly in it.
static noinline bool fast_window(CPUI386 *cpu, uword eip, const u8 **pc, const u8 **lo, uword *len, uintptr_t *ipd)
{
	uword lin = cpu->seg[SEG_CS].base + eip;
	u8 *host, *start;
	uword size;
	if (cpu->cr0 & CR0_PG) {
		struct ftlb_entry *e = &ftlb[(lin >> 12) & (FTLB_N - 1)];
		if (e->rtag == lin >> 12)
			host = (u8 *) (uintptr_t) (lin + e->delta);
		else if (!(host = ftlb_miss(cpu, lin, 1, 0)))
			return false;
		start = host - (lin & 0xfff);
		size = 4096;
	} else {
		uword b0, b1;
		if (lin < 0xa0000) {
			b0 = 0;
			b1 = 0xa0000;
		} else if (lin >= 0xc0000 && lin < (uword) cpu->phys_mem_size) {
			b0 = 0xc0000;
			b1 = cpu->phys_mem_size;
		} else {
			return false;
		}
		start = cpu->phys_mem + b0;
		host = cpu->phys_mem + lin;
		size = b1 - b0;
	}
	if ((uword) (host - start) >= size - 15)
		return false;
	*pc = host;
	*lo = start;
	*len = size - 15;
	*ipd = (uintptr_t) host - eip;
	return true;
}

// Run up to stepcount instructions, as tiny386's cpu_exec1: false on an exception (cpu->excno, cpu->ip at the
// instruction), for cpui386_step to deliver.
static bool T386_FAST_IRAM fast_exec(CPUI386 *cpu, int stepcount)
{
	static const void *const ops[256] = {
		[0 ... 255] = &&slow,
		[0x00] = &&op_00, [0x01] = &&op_01, [0x02] = &&op_02, [0x03] = &&op_03, [0x04] = &&op_04, [0x05] = &&op_05,
		[0x06] = &&op_push_seg, [0x07] = &&op_pop_seg,
		[0x08] = &&op_08, [0x09] = &&op_09, [0x0a] = &&op_0a, [0x0b] = &&op_0b, [0x0c] = &&op_0c, [0x0d] = &&op_0d,
		[0x0e] = &&op_push_seg, [0x0f] = &&op_0f,
		[0x10] = &&op_10, [0x11] = &&op_11, [0x12] = &&op_12, [0x13] = &&op_13, [0x14] = &&op_14, [0x15] = &&op_15,
		[0x16] = &&op_push_seg, [0x17] = &&op_pop_seg,
		[0x18] = &&op_18, [0x19] = &&op_19, [0x1a] = &&op_1a, [0x1b] = &&op_1b, [0x1c] = &&op_1c, [0x1d] = &&op_1d,
		[0x1e] = &&op_push_seg, [0x1f] = &&op_pop_seg,
		[0x20] = &&op_20, [0x21] = &&op_21, [0x22] = &&op_22, [0x23] = &&op_23, [0x24] = &&op_24, [0x25] = &&op_25,
		[0x26] = &&pfx_es,
		[0x28] = &&op_28, [0x29] = &&op_29, [0x2a] = &&op_2a, [0x2b] = &&op_2b, [0x2c] = &&op_2c, [0x2d] = &&op_2d,
		[0x2e] = &&pfx_cs,
		[0x30] = &&op_30, [0x31] = &&op_31, [0x32] = &&op_32, [0x33] = &&op_33, [0x34] = &&op_34, [0x35] = &&op_35,
		[0x36] = &&pfx_ss,
		[0x38] = &&op_38, [0x39] = &&op_39, [0x3a] = &&op_3a, [0x3b] = &&op_3b, [0x3c] = &&op_3c, [0x3d] = &&op_3d,
		[0x3e] = &&pfx_ds,
		[0x40 ... 0x47] = &&op_inc_r, [0x48 ... 0x4f] = &&op_dec_r,
		[0x50 ... 0x57] = &&op_push_r, [0x58 ... 0x5f] = &&op_pop_r,
		[0x60] = &&op_pusha, [0x61] = &&op_popa, [0x63] = &&op_arpl,
		[0x64] = &&pfx_fs, [0x65] = &&pfx_gs, [0x66] = &&pfx_opsz, [0x67] = &&pfx_adsz,
		[0x68] = &&op_push_iv, [0x69] = &&op_imul_iv, [0x6a] = &&op_push_ib, [0x6b] = &&op_imul_ib,
		[0x70 ... 0x7f] = &&op_jcc_b,
		[0x80] = &&op_grp1_b, [0x81] = &&op_grp1_v, [0x82] = &&op_grp1_b, [0x83] = &&op_grp1_vb,
		[0x84] = &&op_84, [0x85] = &&op_85, [0x86] = &&op_86, [0x87] = &&op_87,
		[0x88] = &&op_88, [0x89] = &&op_89, [0x8a] = &&op_8a, [0x8b] = &&op_8b,
		[0x8c] = &&op_8c, [0x8d] = &&op_8d, [0x8e] = &&op_8e, [0x8f] = &&op_8f,
		[0x90] = &&op_nop, [0x91 ... 0x97] = &&op_xchg_ax,
		[0x98] = &&op_cbw, [0x99] = &&op_cwd, [0x9a] = &&op_callf,
		[0x9c] = &&op_pushf, [0x9d] = &&op_popf, [0x9e] = &&op_sahf, [0x9f] = &&op_lahf,
		[0xa0] = &&op_a0, [0xa1] = &&op_a1, [0xa2] = &&op_a2, [0xa3] = &&op_a3,
		[0xa4] = &&op_movsb, [0xa5] = &&op_movs, [0xa6] = &&op_cmpsb, [0xa7] = &&op_cmps,
		[0xa8] = &&op_a8, [0xa9] = &&op_a9,
		[0xaa] = &&op_stosb, [0xab] = &&op_stos, [0xac] = &&op_lodsb, [0xad] = &&op_lods,
		[0xae] = &&op_scasb, [0xaf] = &&op_scas,
		[0xb0 ... 0xb7] = &&op_mov_rb, [0xb8 ... 0xbf] = &&op_mov_rv,
		[0xc0] = &&op_grp2_b, [0xc1] = &&op_grp2_v, [0xc2] = &&op_ret_iw, [0xc3] = &&op_ret,
		[0xc4] = &&op_les, [0xc5] = &&op_lds, [0xc6] = &&op_c6, [0xc7] = &&op_c7,
		[0xc8] = &&op_enter, [0xc9] = &&op_leave, [0xca] = &&op_retf, [0xcb] = &&op_retf,
		[0xcd] = &&op_int, [0xcf] = &&op_iret,
		[0xd0] = &&op_grp2_b, [0xd1] = &&op_grp2_v, [0xd2] = &&op_grp2_b, [0xd3] = &&op_grp2_v,
		[0xd7] = &&op_xlat,
		[0xe0] = &&op_loopne, [0xe1] = &&op_loope, [0xe2] = &&op_loop, [0xe3] = &&op_jcxz,
		[0xe4] = &&op_in_ib, [0xe5] = &&op_in_ib, [0xe6] = &&op_out_ib, [0xe7] = &&op_out_ib,
		[0xe8] = &&op_call, [0xe9] = &&op_jmp_v, [0xea] = &&op_jmpf, [0xeb] = &&op_jmp_b,
		[0xec] = &&op_in_dx, [0xed] = &&op_in_dx, [0xee] = &&op_out_dx, [0xef] = &&op_out_dx,
		[0xf0] = &&pfx_lock, [0xf2] = &&pfx_repne, [0xf3] = &&pfx_rep,
		[0xf5] = &&op_cmc, [0xf6] = &&op_grp3_b, [0xf7] = &&op_grp3_v,
		[0xf8] = &&op_clc, [0xf9] = &&op_stc, [0xfa] = &&op_cli, [0xfb] = &&op_sti,
		[0xfc] = &&op_cld, [0xfd] = &&op_std, [0xfe] = &&op_grp4, [0xff] = &&op_grp5,
	};
	static const void *const ops0f[256] = {
		[0 ... 255] = &&slow,
		[0x80 ... 0x8f] = &&op_jcc_v, [0x90 ... 0x9f] = &&op_setcc,
		[0xa0] = &&op_push_seg, [0xa1] = &&op_pop_seg, [0xa3] = &&op_bt, [0xa4] = &&op_shld, [0xa5] = &&op_shld,
		[0xa8] = &&op_push_seg, [0xa9] = &&op_pop_seg, [0xab] = &&op_bt, [0xac] = &&op_shrd, [0xad] = &&op_shrd,
		[0xaf] = &&op_imul_r, [0xb3] = &&op_bt, [0xb6] = &&op_movzx_b, [0xb7] = &&op_movzx_w,
		[0xba] = &&op_bt_ib, [0xbb] = &&op_bt, [0xbc] = &&op_bsf, [0xbd] = &&op_bsr,
		[0xbe] = &&op_movsx_b, [0xbf] = &&op_movsx_w,
	};

	// virtual-8086 mode as real mode, but for what it traps or checks (INT, IRET, I/O, and with IOPL < 3 the
	// interrupt flag's instructions): tiny386. A change of mode leaves (vm0).
	const uword vm0 = cpu->flags & VM;

	int steps = stepcount - 1;   // instructions left after the current one
	int nslow = 0;            // instructions passed to tiny386 (which counts its own)
	const u8 *pc;             // the next instruction byte (host address)
	const u8 *ins;            // the current instruction's first byte
	const u8 *win_lo;         // the code window: instructions may start in [win_lo, win_lo + win_len)
	uword win_len;
	uintptr_t ipd;            // eip = pc - ipd
	bool code16;
	uword sp_mask;
	unsigned nullsegs;
	// the current instruction's prefixes: bit 0 16-bit operands, bit 1 16-bit addresses, bits 2-3 REP (1) or REPNE
	// (2), bits 4-6 the segment override + 1; pfx0 without prefixes
	unsigned pfx, pfx0;
#define opsz16 (pfx & 1)
#define adsz16 ((pfx >> 1) & 1)
#define rep ((pfx >> 2) & 3)
#define SEG_OVERRIDE ((int) (pfx >> 4) - 1)
	int seg = -1;             // a memory operand's segment
	int strn;                 // string instructions: the element size
	uword eip;
	uword mlin;               // an MMIO access: its linear address, size, value (stores), register (loads)
	u32 mval;
	int msize, mreg;

#define EIP_OF(p) ((uword) ((uintptr_t) (p) - ipd))
	// every handler ends dispatching the next instruction itself (so that the host's branch predictor sees
	// one indirect jump per handler)
#define DISPATCH do { ins = pc; pfx = pfx0; goto *ops[*pc++]; } while (0)
#define NEXT do { \
		if (unlikely(--steps < 0)) goto out; \
		if (unlikely((uword) (pc - win_lo) >= win_len)) goto refill; \
		DISPATCH; \
	} while (0)
	// relative and absolute jumps; 16-bit code wraps at 64 KB
#define JUMP_REL(d) do { \
		if (code16) pc = (const u8 *) (ipd + ((EIP_OF(pc) + (d)) & 0xffff)); \
		else pc += (d); \
	} while (0)
#define JUMP_ABS(neweip) (pc = (const u8 *) (ipd + (uword) (neweip)))
	// a memory operand's host address, or the instruction goes to tiny386
#define FPTR(p, sg, off, size, wr) do { \
		if (unlikely(nullsegs) && (nullsegs & (1 << (sg)))) goto slow; \
		uword _lin = cpu->seg[sg].base + (off); \
		uword _pg = _lin >> 12; \
		struct ftlb_entry *_e = &ftlb[_pg & (FTLB_N - 1)]; \
		if (likely(((wr) ? _e->wtag : _e->rtag) == _pg && \
			   ((size) == 1 || (_lin & 0xfff) <= 0x1000 - (size)))) \
			p = (u8 *) (uintptr_t) (_lin + _e->delta); \
		else if (!(p = ftlb_miss(cpu, _lin, (size), (wr)))) \
			goto slow; \
	} while (0)
	// as FPTR, but an access that isn't to RAM goes to the label (mlin, msize set) to be tried as MMIO
#define FPTR_IO(p, sg, off, size, wr, LABEL) do { \
		if (unlikely(nullsegs) && (nullsegs & (1 << (sg)))) goto slow; \
		uword _lin = cpu->seg[sg].base + (off); \
		uword _pg = _lin >> 12; \
		struct ftlb_entry *_e = &ftlb[_pg & (FTLB_N - 1)]; \
		if (likely(((wr) ? _e->wtag : _e->rtag) == _pg && \
			   ((size) == 1 || (_lin & 0xfff) <= 0x1000 - (size)))) \
			p = (u8 *) (uintptr_t) (_lin + _e->delta); \
		else if (!(p = ftlb_miss(cpu, _lin, (size), (wr)))) { \
			mlin = _lin; \
			msize = (size); \
			goto LABEL; \
		} \
	} while (0)
#define EA_OFF(off, m) \
	seg = SEG_OVERRIDE; \
	uword off = fast_ea(cpu, &pc, m, adsz16, &seg);
#define EA_PTR(p, m, size, wr) \
	seg = SEG_OVERRIDE; \
	uword _off = fast_ea(cpu, &pc, m, adsz16, &seg); \
	FPTR(p, seg, _off, size, wr)
	// stack slots: the stack pointer's address after moving it by d bytes
#define STACK_PTR(p, d, size, wr) do { \
		uword _sp = (REGi(4) + (d)) & sp_mask; \
		if ((size) > 4 && _sp > sp_mask - ((size) - 1)) goto slow;   /* slots wrapping around: tiny386 */ \
		FPTR(p, SEG_SS, _sp, size, wr); \
	} while (0)
#define SP_ADD(d) set_sp(REGi(4) + (d), sp_mask)
#define PMODE (cpu->cr0 & 1)                          // protected mode, virtual-8086 mode included
#define PROTECTED (PMODE && !(cpu->flags & VM))       // ... with descriptors (segment loads, far transfers)
#define V86_IOPL3 (!(cpu->flags & VM) || get_IOPL(cpu) == 3)   // not trapped in virtual-8086 mode
// a protected-mode load of DS ES FS or GS (tiny386's set_seg); false: the instruction goes to tiny386
#define FAST_LOAD_SEG(sg, sel) (set_seg(cpu, (sg), (sel)) ? (nullsegs = fast_nullsegs(cpu), true) : false)
	// the instructions done here, for cpui386_get_cycle (approximate: STI and MOV SS adjust steps)
#define FAST_COUNT() do { int _n = stepcount - 1 - steps - nslow; if (_n > 0) cpu->cycle += _n; } while (0)
	// after an instruction that changed CS (eip: the next one)
#define RELOAD do { \
		if (--steps < 0) { cpu->next_ip = eip; goto out_ip; } \
		goto reload; \
	} while (0)
	// a protected-mode far transfer (CALL, JMP, RETF, IRET) by tiny386's pmcall/pmret, which do the checks, the
	// pushes and pops, gates and stack switches, and set CS and next_ip; a fault is taken as tiny386 takes it: the
	// instruction undone to its start (cpu->ip), the exception raised by cpui386_step
#define PM_FAR(call) do { \
		cpu->ip = EIP_OF(ins); \
		cpu->next_ip = EIP_OF(pc); \
		if (!(call)) { FAST_COUNT(); return false; } \
		ftlb_check(cpu); \
		eip = cpu->next_ip; \
		if ((cpu->flags & VM) != vm0 || (cpu->intr && (cpu->flags & IF))) { goto out_ip; } \
		RELOAD; \
	} while (0)

	eip = cpu->next_ip;
	cpu->ifetch.paddr = 0;
	ftlb_check(cpu);
reload:   // after a change of mode, CS or segment registers made outside the handlers below; eip is the next instruction
	code16 = cpu->code16;
	pfx0 = code16 ? 3 : 0;
	sp_mask = cpu->sp_mask;
	nullsegs = fast_nullsegs(cpu);
	if (!fast_window(cpu, eip, &pc, &win_lo, &win_len, &ipd))
		goto slow_eip;
	DISPATCH;
refill:   // the next instruction is outside the code window
	eip = EIP_OF(pc);
	if (!fast_window(cpu, eip, &pc, &win_lo, &win_len, &ipd))
		goto slow_eip;
	DISPATCH;

	// ---- prefixes
#define SEG_PREFIX(s) pfx = (pfx & 0x0f) | (((s) + 1) << 4); goto *ops[*pc++];
pfx_es: SEG_PREFIX(SEG_ES)
pfx_cs: SEG_PREFIX(SEG_CS)
pfx_ss: SEG_PREFIX(SEG_SS)
pfx_ds: SEG_PREFIX(SEG_DS)
pfx_fs: SEG_PREFIX(SEG_FS)
pfx_gs: SEG_PREFIX(SEG_GS)
pfx_opsz: pfx = (pfx & ~1u) | ((pfx0 & 1) ^ 1); goto *ops[*pc++];
pfx_adsz: pfx = (pfx & ~2u) | ((pfx0 & 2) ^ 2); goto *ops[*pc++];
pfx_rep: pfx = (pfx & ~0xcu) | (1 << 2); goto *ops[*pc++];
pfx_repne: pfx = (pfx & ~0xcu) | (2 << 2); goto *ops[*pc++];
pfx_lock: goto *ops[*pc++];
op_0f: goto *ops0f[*pc++];

	// ---- ALU: op Eb,Gb / Ev,Gv / Gb,Eb / Gv,Ev / AL,Ib / eAX,Iv
#define ALU6(L0, L1, L2, L3, L4, L5, OP, OPb, OPw, OPd, WR) \
L0: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7; \
	if (m >= 0xc0) { OPb(rm, r, fr8, fw8, fr8, fw8) } \
	else { u8 *p; EA_PTR(p, m, 1, WR); OPb(p, r, fm8, fs8, fr8, fw8) } \
	NEXT; } \
L1: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7; \
	if (m >= 0xc0) { \
		if (opsz16) { OPw(rm, r, lreg16, sreg16, lreg16, sreg16) } \
		else { OPd(rm, r, lreg32, sreg32, lreg32, sreg32) } \
	} else if (opsz16) { u8 *p; EA_PTR(p, m, 2, WR); OPw(p, r, fm16, fs16, lreg16, sreg16) } \
	else { u8 *p; EA_PTR(p, m, 4, WR); OPd(p, r, fm32, fs32, lreg32, sreg32) } \
	NEXT; } \
L2: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7; \
	if (m >= 0xc0) { OPb(r, rm, fr8, fw8, fr8, fw8) } \
	else { u8 *p; EA_PTR(p, m, 1, 0); OPb(r, p, fr8, fw8, fm8, fs8) } \
	NEXT; } \
L3: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7; \
	if (m >= 0xc0) { \
		if (opsz16) { OPw(r, rm, lreg16, sreg16, lreg16, sreg16) } \
		else { OPd(r, rm, lreg32, sreg32, lreg32, sreg32) } \
	} else if (opsz16) { u8 *p; EA_PTR(p, m, 2, 0); OPw(r, p, lreg16, sreg16, fm16, fs16) } \
	else { u8 *p; EA_PTR(p, m, 4, 0); OPd(r, p, lreg32, sreg32, fm32, fs32) } \
	NEXT; } \
L4: { u8 i = *pc++; int r = 0; OPb(r, i, fr8, fw8, limm, 0) NEXT; } \
L5: { int r = 0; \
	if (opsz16) { u16 i = *(u16a *) pc; pc += 2; OPw(r, i, lreg16, sreg16, limm, 0) } \
	else { u32 i = *(u32a *) pc; pc += 4; OPd(r, i, lreg32, sreg32, limm, 0) } \
	NEXT; }

	ALU6(op_00, op_01, op_02, op_03, op_04, op_05, ADD, ADDb, ADDw, ADDd, 1)
	ALU6(op_08, op_09, op_0a, op_0b, op_0c, op_0d, OR, ORb, ORw, ORd, 1)
	ALU6(op_10, op_11, op_12, op_13, op_14, op_15, ADC, ADCb, ADCw, ADCd, 1)
	ALU6(op_18, op_19, op_1a, op_1b, op_1c, op_1d, SBB, SBBb, SBBw, SBBd, 1)
	ALU6(op_20, op_21, op_22, op_23, op_24, op_25, AND, ANDb, ANDw, ANDd, 1)
	ALU6(op_28, op_29, op_2a, op_2b, op_2c, op_2d, SUB, SUBb, SUBw, SUBd, 1)
	ALU6(op_30, op_31, op_32, op_33, op_34, op_35, XOR, XORb, XORw, XORd, 1)
	ALU6(op_38, op_39, op_3a, op_3b, op_3c, op_3d, CMP, FCMPb, FCMPw, FCMPd, 0)

	// TEST, XCHG, MOV with modrm
op_84: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7;
	if (m >= 0xc0) { FTESTb(rm, r, fr8, fw8, fr8, fw8) }
	else { u8 *p; EA_PTR(p, m, 1, 0); FTESTb(p, r, fm8, fs8, fr8, fw8) }
	NEXT; }
op_85: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7;
	if (m >= 0xc0) {
		if (opsz16) { FTESTw(rm, r, lreg16, sreg16, lreg16, sreg16) }
		else { FTESTd(rm, r, lreg32, sreg32, lreg32, sreg32) }
	} else if (opsz16) { u8 *p; EA_PTR(p, m, 2, 0); FTESTw(p, r, fm16, fs16, lreg16, sreg16) }
	else { u8 *p; EA_PTR(p, m, 4, 0); FTESTd(p, r, fm32, fs32, lreg32, sreg32) }
	NEXT; }
op_86: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7;
	if (m >= 0xc0) { XCHGb(rm, r, fr8, fw8, fr8, fw8) }
	else { u8 *p; EA_PTR(p, m, 1, 1); XCHGb(p, r, fm8, fs8, fr8, fw8) }
	NEXT; }
op_87: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7;
	if (m >= 0xc0) {
		if (opsz16) { XCHGw(rm, r, lreg16, sreg16, lreg16, sreg16) }
		else { XCHGd(rm, r, lreg32, sreg32, lreg32, sreg32) }
	} else if (opsz16) { u8 *p; EA_PTR(p, m, 2, 1); XCHGw(p, r, fm16, fs16, lreg16, sreg16) }
	else { u8 *p; EA_PTR(p, m, 4, 1); XCHGd(p, r, fm32, fs32, lreg32, sreg32) }
	NEXT; }
op_88: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7;
	if (m >= 0xc0) fw8(rm, fr8(r));
	else { EA_OFF(off, m) mval = fr8(r); u8 *p; FPTR_IO(p, seg, off, 1, 1, mmio_st); fs8(p, mval); }
	NEXT; }
op_89: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7;
	if (m >= 0xc0) {
		if (opsz16) sreg16(rm, lreg16(r));
		else sreg32(rm, lreg32(r));
	} else if (opsz16) { EA_OFF(off, m) mval = lreg16(r); u8 *p; FPTR_IO(p, seg, off, 2, 1, mmio_st); fs16(p, mval); }
	else { EA_OFF(off, m) mval = lreg32(r); u8 *p; FPTR_IO(p, seg, off, 4, 1, mmio_st); fs32(p, mval); }
	NEXT; }
op_8a: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7;
	if (m >= 0xc0) fw8(r, fr8(rm));
	else { EA_OFF(off, m) mreg = r; u8 *p; FPTR_IO(p, seg, off, 1, 0, mmio_ld); fw8(r, fm8(p)); }
	NEXT; }
op_8b: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7;
	if (m >= 0xc0) {
		if (opsz16) sreg16(r, lreg16(rm));
		else sreg32(r, lreg32(rm));
	} else if (opsz16) { EA_OFF(off, m) mreg = r; u8 *p; FPTR_IO(p, seg, off, 2, 0, mmio_ld); sreg16(r, fm16(p)); }
	else { EA_OFF(off, m) mreg = r; u8 *p; FPTR_IO(p, seg, off, 4, 0, mmio_ld); sreg32(r, fm32(p)); }
	NEXT; }
	// MMIO for the MOVs above (mlin, msize; mval to store, mreg to load into)
mmio_st: {
	uword pa = fast_mmio_addr(cpu, mlin, msize, 1);
	if (pa == (uword) -1) goto slow;
	fast_io_write(cpu, pa, msize, mval);
	NEXT; }
mmio_ld: {
	uword pa = fast_mmio_addr(cpu, mlin, msize, 0);
	if (pa == (uword) -1) goto slow;
	u32 v = fast_io_read(cpu, pa, msize);
	if (msize == 1) fw8(mreg, v); else if (msize == 2) sreg16(mreg, v); else sreg32(mreg, v);
	NEXT; }
op_8c: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7;   // MOV Ew,Sw
	if (r > 5) goto slow;
	if (m >= 0xc0) {
		if (opsz16) sreg16(rm, cpu->seg[r].sel);
		else sreg32(rm, cpu->seg[r].sel);
	} else { u8 *p; EA_PTR(p, m, 2, 1); fs16(p, cpu->seg[r].sel); }
	NEXT; }
op_8d: { u8 m = *pc++; int r = (m >> 3) & 7;   // LEA
	if (m >= 0xc0) goto slow;
	uword a = fast_ea(cpu, &pc, m, adsz16, &seg);   // (seg is unused)
	if (opsz16) sreg16(r, a);
	else sreg32(r, a);
	NEXT; }
op_8e: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7;   // MOV Sw,Ew (protected mode: DS ES FS GS)
	if (r == SEG_CS || r > 5 || (PROTECTED && r == SEG_SS)) goto slow;
	u16 sel;
	if (m >= 0xc0) sel = lreg16(rm);
	else { u8 *p; EA_PTR(p, m, 2, 0); sel = fm16(p); }
	if (PROTECTED) {
		if (!FAST_LOAD_SEG(r, sel)) goto slow;
		NEXT;
	}
	cpu->seg[r].sel = sel;
	cpu->seg[r].base = sel << 4;
	cpu->seg[r].limit = 0xffff;
	cpu->seg[r].flags = 0;
	if (r == SEG_SS) {
		sp_mask = cpu->sp_mask = 0xffff;
		steps++;   // no interrupt before the next instruction (tiny386: stepcount++)
	}
	NEXT; }
op_8f: { u8 m = *pc++; int rm = m & 7;   // POP Ev
	if (m < 0xc0) {   // to memory: its address with (E)SP moved on (as tiny386), stored before (E)SP moves
		if (((m >> 3) & 7) != 0) goto slow;
		uword sp0 = REGi(4);
		u8 *s, *p;
		if (opsz16) {
			STACK_PTR(s, 0, 2, 0); u16 v = fm16(s);
			SP_ADD(2); EA_OFF(off, m) set_sp(sp0, sp_mask);
			FPTR(p, seg, off, 2, 1); fs16(p, v); SP_ADD(2);
		} else {
			STACK_PTR(s, 0, 4, 0); u32 v = fm32(s);
			SP_ADD(4); EA_OFF(off, m) set_sp(sp0, sp_mask);
			FPTR(p, seg, off, 4, 1); fs32(p, v); SP_ADD(4);
		}
		NEXT;
	}
	if (opsz16) { u8 *s; STACK_PTR(s, 0, 2, 0); u16 v = fm16(s); SP_ADD(2); sreg16(rm, v); }
	else { u8 *s; STACK_PTR(s, 0, 4, 0); u32 v = fm32(s); SP_ADD(4); sreg32(rm, v); }
	NEXT; }

	// MOV r,imm; MOV E,imm; MOV with a direct offset
op_mov_rb: { int r = pc[-1] & 7; fw8(r, *pc++); NEXT; }
op_mov_rv: { int r = pc[-1] & 7;
	if (opsz16) { sreg16(r, *(u16a *) pc); pc += 2; }
	else { sreg32(r, *(u32a *) pc); pc += 4; }
	NEXT; }
op_c6: { u8 m = *pc++; int rm = m & 7;
	if (((m >> 3) & 7) != 0) goto slow;
	if (m >= 0xc0) fw8(rm, *pc++);
	else { EA_OFF(off, m) mval = *pc++; u8 *p; FPTR_IO(p, seg, off, 1, 1, mmio_st); fs8(p, mval); }
	NEXT; }
op_c7: { u8 m = *pc++; int rm = m & 7;
	if (((m >> 3) & 7) != 0) goto slow;
	if (m >= 0xc0) {
		if (opsz16) { sreg16(rm, *(u16a *) pc); pc += 2; }
		else { sreg32(rm, *(u32a *) pc); pc += 4; }
	} else if (opsz16) { EA_OFF(off, m) mval = *(u16a *) pc; pc += 2; u8 *p; FPTR_IO(p, seg, off, 2, 1, mmio_st); fs16(p, mval); }
	else { EA_OFF(off, m) mval = *(u32a *) pc; pc += 4; u8 *p; FPTR_IO(p, seg, off, 4, 1, mmio_st); fs32(p, mval); }
	NEXT; }
#define MOFFS(off) \
	uword off; \
	if (adsz16) { off = *(u16a *) pc; pc += 2; } else { off = *(u32a *) pc; pc += 4; } \
	seg = SEG_OVERRIDE < 0 ? SEG_DS : SEG_OVERRIDE;
op_a0: { MOFFS(off) mreg = 0; u8 *p; FPTR_IO(p, seg, off, 1, 0, mmio_ld); fw8(0, fm8(p)); NEXT; }
op_a1: { MOFFS(off) mreg = 0;
	if (opsz16) { u8 *p; FPTR_IO(p, seg, off, 2, 0, mmio_ld); sreg16(0, fm16(p)); }
	else { u8 *p; FPTR_IO(p, seg, off, 4, 0, mmio_ld); sreg32(0, fm32(p)); }
	NEXT; }
op_a2: { MOFFS(off) mval = fr8(0); u8 *p; FPTR_IO(p, seg, off, 1, 1, mmio_st); fs8(p, mval); NEXT; }
op_a3: { MOFFS(off)
	if (opsz16) { mval = lreg16(0); u8 *p; FPTR_IO(p, seg, off, 2, 1, mmio_st); fs16(p, mval); }
	else { mval = lreg32(0); u8 *p; FPTR_IO(p, seg, off, 4, 1, mmio_st); fs32(p, mval); }
	NEXT; }
op_a8: { u8 i = *pc++; int r = 0; FTESTb(r, i, fr8, fw8, limm, 0) NEXT; }
op_a9: { int r = 0;
	if (opsz16) { u16 i = *(u16a *) pc; pc += 2; FTESTw(r, i, lreg16, sreg16, limm, 0) }
	else { u32 i = *(u32a *) pc; pc += 4; FTESTd(r, i, lreg32, sreg32, limm, 0) }
	NEXT; }

	// INC, DEC, PUSH, POP of registers
op_inc_r: { int r = pc[-1] & 7;
	if (opsz16) { INCw(r, lreg16, sreg16) } else { INCd(r, lreg32, sreg32) }
	NEXT; }
op_dec_r: { int r = pc[-1] & 7;
	if (opsz16) { DECw(r, lreg16, sreg16) } else { DECd(r, lreg32, sreg32) }
	NEXT; }
op_push_r: { int r = pc[-1] & 7;
	if (opsz16) { u8 *s; STACK_PTR(s, -2, 2, 1); u16 v = lreg16(r); SP_ADD(-2); fs16(s, v); }
	else { u8 *s; STACK_PTR(s, -4, 4, 1); u32 v = lreg32(r); SP_ADD(-4); fs32(s, v); }
	NEXT; }
op_pop_r: { int r = pc[-1] & 7;
	if (opsz16) { u8 *s; STACK_PTR(s, 0, 2, 0); u16 v = fm16(s); SP_ADD(2); sreg16(r, v); }
	else { u8 *s; STACK_PTR(s, 0, 4, 0); u32 v = fm32(s); SP_ADD(4); sreg32(r, v); }
	NEXT; }
op_push_iv: {
	if (opsz16) { u8 *s; STACK_PTR(s, -2, 2, 1); SP_ADD(-2); fs16(s, *(u16a *) pc); pc += 2; }
	else { u8 *s; STACK_PTR(s, -4, 4, 1); SP_ADD(-4); fs32(s, *(u32a *) pc); pc += 4; }
	NEXT; }
op_push_ib: { uword v = (sword) (s8) *pc++;
	if (opsz16) { u8 *s; STACK_PTR(s, -2, 2, 1); SP_ADD(-2); fs16(s, v); }
	else { u8 *s; STACK_PTR(s, -4, 4, 1); SP_ADD(-4); fs32(s, v); }
	NEXT; }
op_pusha: {
	if (opsz16) {
		u8 *s; STACK_PTR(s, -16, 16, 1);
		uword sp = REGi(4);
		for (int i = 0; i < 8; i++)
			fs16(s + 14 - 2 * i, i == 4 ? sp : lreg16(i));
		SP_ADD(-16);
	} else {
		u8 *s; STACK_PTR(s, -32, 32, 1);
		uword sp = REGi(4);
		for (int i = 0; i < 8; i++)
			fs32(s + 28 - 4 * i, i == 4 ? sp : lreg32(i));
		SP_ADD(-32);
	}
	NEXT; }
op_popa: {
	if (opsz16) {
		u8 *s; STACK_PTR(s, 0, 16, 0);
		for (int i = 0; i < 8; i++)
			if (i != 4) sreg16(i, fm16(s + 14 - 2 * i));
		SP_ADD(16);
	} else {
		u8 *s; STACK_PTR(s, 0, 32, 0);
		for (int i = 0; i < 8; i++)
			if (i != 4) sreg32(i, fm32(s + 28 - 4 * i));
		SP_ADD(32);
	}
	NEXT; }

	// ARPL (16-bit protected-mode code checks selectors with it): protected mode only; the memory operand read
	// first, and written (a write access) only if its RPL goes up, as tiny386 does
op_arpl: { if (!PROTECTED) goto slow;
	u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7;
	u16 src = lreg16(r);
	if (m >= 0xc0) {
		u16 dst = lreg16(rm);
		if ((dst & 3) < (src & 3)) { cpu->flags |= ZF; sreg16(rm, (dst & ~3) | (src & 3)); }
		else cpu->flags &= ~ZF;
	} else {
		EA_OFF(off, m) u8 *p; FPTR(p, seg, off, 2, 0);
		u16 dst = fm16(p);
		if ((dst & 3) < (src & 3)) { FPTR(p, seg, off, 2, 1); cpu->flags |= ZF; fs16(p, (dst & ~3) | (src & 3)); }
		else cpu->flags &= ~ZF;
	}
	cpu->cc.mask &= ~ZF;
	NEXT; }

	// segment registers: PUSH any; POP, and in protected mode DS ES FS GS through tiny386's set_seg (any fault: the
	// whole instruction to tiny386, which raises it; set_seg changes nothing before it has passed its checks)
op_push_seg: {
	u8 op = pc[-1];   // 06 0E 16 1E, or 0F A0 / 0F A8 (primary A0 and A8 are other instructions)
	int sg = op == 0xa0 ? SEG_FS : op == 0xa8 ? SEG_GS : op >> 3;
	if (opsz16) { u8 *s; STACK_PTR(s, -2, 2, 1); SP_ADD(-2); fs16(s, cpu->seg[sg].sel); }
	else { u8 *s; STACK_PTR(s, -4, 2, 1); SP_ADD(-4); fs16(s, cpu->seg[sg].sel); }
	NEXT; }
op_pop_seg: {
	u8 op = pc[-1];
	int sg = op == 0xa1 ? SEG_FS : op == 0xa9 ? SEG_GS : op >> 3;   // 07 17 1F, 0F A1 / 0F A9
	if (PROTECTED && sg == SEG_SS) goto slow;
	u8 *s; STACK_PTR(s, 0, 2, 0);
	u16 sel = fm16(s);
	if (PROTECTED) {
		if (!FAST_LOAD_SEG(sg, sel)) goto slow;
		SP_ADD(opsz16 ? 2 : 4);
		NEXT;
	}
	SP_ADD(opsz16 ? 2 : 4);
	cpu->seg[sg].sel = sel;
	cpu->seg[sg].base = sel << 4;
	cpu->seg[sg].limit = 0xffff;
	cpu->seg[sg].flags = 0;
	if (sg == SEG_SS) {
		sp_mask = cpu->sp_mask = 0xffff;
		steps++;
	}
	NEXT; }
op_les: op_lds: { u8 m = *pc++; int r = (m >> 3) & 7;
	if (m >= 0xc0) goto slow;
	int sg = pc[-2] == 0xc4 ? SEG_ES : SEG_DS;
	u8 *p;
	if (PROTECTED) {   // the selector loaded first (as tiny386): a fault leaves the register as it was
		u32 off;
		u16 sel;
		if (opsz16) { EA_PTR(p, m, 4, 0); off = fm16(p); sel = fm16(p + 2); }
		else { EA_PTR(p, m, 6, 0); off = fm32(p); sel = fm16(p + 4); }
		if (!FAST_LOAD_SEG(sg, sel)) goto slow;
		if (opsz16) sreg16(r, off); else sreg32(r, off);
		NEXT;
	}
	if (opsz16) {
		EA_PTR(p, m, 4, 0);
		sreg16(r, fm16(p));
		u16 sel = fm16(p + 2);
		cpu->seg[sg].sel = sel;
		cpu->seg[sg].base = sel << 4;
	} else {
		EA_PTR(p, m, 6, 0);
		sreg32(r, fm32(p));
		u16 sel = fm16(p + 4);
		cpu->seg[sg].sel = sel;
		cpu->seg[sg].base = sel << 4;
	}
	cpu->seg[sg].limit = 0xffff;
	cpu->seg[sg].flags = 0;
	NEXT; }

	// IMUL with an immediate, PUSHF/POPF, flags
op_imul_iv: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7;
	if (opsz16) {
		if (m >= 0xc0) { u16 i = *(u16a *) pc; pc += 2; IMUL2wIw(r, rm, i, lreg16, sreg16, lreg16, sreg16) }
		else { u8 *p; EA_PTR(p, m, 2, 0); u16 i = *(u16a *) pc; pc += 2; IMUL2wIw(r, p, i, lreg16, sreg16, fm16, fs16) }
	} else {
		if (m >= 0xc0) { u32 i = *(u32a *) pc; pc += 4; IMUL2dId(r, rm, i, lreg32, sreg32, lreg32, sreg32) }
		else { u8 *p; EA_PTR(p, m, 4, 0); u32 i = *(u32a *) pc; pc += 4; IMUL2dId(r, p, i, lreg32, sreg32, fm32, fs32) }
	}
	NEXT; }
op_imul_ib: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7;
	if (opsz16) {
		if (m >= 0xc0) { u8 i = *pc++; IMUL2wIb(r, rm, i, lreg16, sreg16, lreg16, sreg16) }
		else { u8 *p; EA_PTR(p, m, 2, 0); u8 i = *pc++; IMUL2wIb(r, p, i, lreg16, sreg16, fm16, fs16) }
	} else {
		if (m >= 0xc0) { u8 i = *pc++; IMUL2dIb(r, rm, i, lreg32, sreg32, lreg32, sreg32) }
		else { u8 *p; EA_PTR(p, m, 4, 0); u8 i = *pc++; IMUL2dIb(r, p, i, lreg32, sreg32, fm32, fs32) }
	}
	NEXT; }
op_pushf: {
	if (!V86_IOPL3) goto slow;
	if (opsz16) { u8 *s; STACK_PTR(s, -2, 2, 1); refresh_flags(cpu); cpu->cc.mask = 0; SP_ADD(-2); fs16(s, cpu->flags); }
	else { u8 *s; STACK_PTR(s, -4, 4, 1); refresh_flags(cpu); cpu->cc.mask = 0; SP_ADD(-4); fs32(s, cpu->flags & ~(RF | VM)); }
	NEXT; }
op_popf: {
	if (!V86_IOPL3) goto slow;
	uword mask = VM;
	if (PMODE) {
		if (cpu->cpl > 0) mask |= IOPL;
		if (get_IOPL(cpu) < cpu->cpl) mask |= IF;
	}
	if (opsz16) {
		u8 *s; STACK_PTR(s, 0, 2, 0);
		SP_ADD(2);
		cpu->flags = (cpu->flags & (0xffff0000 | mask)) | (fm16(s) & ~mask);
	} else {
		u8 *s; STACK_PTR(s, 0, 4, 0);
		SP_ADD(4);
		cpu->flags = (cpu->flags & mask) | (fm32(s) & ~mask);
	}
	cpu->flags &= EFLAGS_MASK;
	cpu->flags |= 0x2;
	cpu->cc.mask = 0;
	if (cpu->intr && (cpu->flags & IF)) {   // as tiny386: the interrupt now
		goto out;
	}
	NEXT; }
op_sahf: { SAHF() NEXT; }
op_lahf: { LAHF() NEXT; }
op_cmc: { CMC() NEXT; }
op_clc: { CLC() NEXT; }
op_stc: { STC() NEXT; }
op_cld: { CLD() NEXT; }
op_std: { STD() NEXT; }
op_cli: {
	if (get_IOPL(cpu) < cpu->cpl) goto slow;
	cpu->flags &= ~IF;
	NEXT; }
op_sti: {
	if (get_IOPL(cpu) < cpu->cpl) goto slow;
	cpu->flags |= IF;
	if (cpu->intr || steps < 1) steps = 1;   // interrupts from after the next instruction
	NEXT; }
op_nop: NEXT;
op_xchg_ax: { int r = pc[-1] & 7;
	if (opsz16) { u16 t = lreg16(r); sreg16(r, lreg16(0)); sreg16(0, t); }
	else { u32 t = lreg32(r); sreg32(r, lreg32(0)); sreg32(0, t); }
	NEXT; }
op_cbw: { CBW_CWDE() NEXT; }
op_cwd: { CWD_CDQ() NEXT; }
op_xlat: {
	seg = SEG_OVERRIDE < 0 ? SEG_DS : SEG_OVERRIDE;
	uword off = adsz16 ? (uword) ((lreg16(3) + fr8(0)) & 0xffff) : lreg32(3) + fr8(0);
	u8 *p; FPTR(p, seg, off, 1, 0);
	fw8(0, fm8(p));
	NEXT; }

	// jumps, calls, returns, loops
op_jcc_b: { s8 d = *pc++;
	if (fast_cond(cpu, pc[-2] & 15)) JUMP_REL(d);
	NEXT; }
op_jcc_v: {   // rel16/32 by the operand size (tiny386's Jv: vendor/patches/0008)
	int c = pc[-1] & 15;
	sword d;
	if (opsz16) { d = (s16) *(u16a *) pc; pc += 2; } else { d = (s32) *(u32a *) pc; pc += 4; }
	if (fast_cond(cpu, c)) JUMP_REL(d);
	NEXT; }
op_setcc: { u8 m = *pc++; int rm = m & 7;
	int c = fast_cond(cpu, pc[-2] & 15);
	if (m >= 0xc0) fw8(rm, c);
	else { u8 *p; EA_PTR(p, m, 1, 1); fs8(p, c); }
	NEXT; }
op_jmp_b: { s8 d = *pc++; JUMP_REL(d); NEXT; }
op_jmp_v: {   // Jv: as JCC above
	sword d;
	if (opsz16) { d = (s16) *(u16a *) pc; pc += 2; } else { d = (s32) *(u32a *) pc; pc += 4; }
	JUMP_REL(d);
	NEXT; }
op_call: {
	sword d;
	if (opsz16) {
		d = (s16) *(u16a *) pc; pc += 2;
		u8 *s; STACK_PTR(s, -2, 2, 1);
		SP_ADD(-2); fs16(s, EIP_OF(pc));
	} else {
		d = (s32) *(u32a *) pc; pc += 4;
		u8 *s; STACK_PTR(s, -4, 4, 1);
		SP_ADD(-4); fs32(s, EIP_OF(pc));
	}
	JUMP_REL(d);
	NEXT; }
op_ret: {
	if (opsz16) { u8 *s; STACK_PTR(s, 0, 2, 0); SP_ADD(2); JUMP_ABS(fm16(s)); }
	else { u8 *s; STACK_PTR(s, 0, 4, 0); SP_ADD(4); JUMP_ABS(fm32(s)); }
	NEXT; }
op_ret_iw: { u16 n = *(u16a *) pc; pc += 2;
	if (opsz16) { u8 *s; STACK_PTR(s, 0, 2, 0); SP_ADD(2 + n); JUMP_ABS(fm16(s)); }
	else { u8 *s; STACK_PTR(s, 0, 4, 0); SP_ADD(4 + n); JUMP_ABS(fm32(s)); }
	NEXT; }
op_loop: { s8 d = *pc++;
	if (adsz16) { u16 c = lreg16(1) - 1; sreg16(1, c); if (c) JUMP_REL(d); }
	else { u32 c = lreg32(1) - 1; sreg32(1, c); if (c) JUMP_REL(d); }
	NEXT; }
op_loope: { s8 d = *pc++;
	if (adsz16) { u16 c = lreg16(1) - 1; sreg16(1, c); if (c && get_ZF(cpu)) JUMP_REL(d); }
	else { u32 c = lreg32(1) - 1; sreg32(1, c); if (c && get_ZF(cpu)) JUMP_REL(d); }
	NEXT; }
op_loopne: { s8 d = *pc++;
	if (adsz16) { u16 c = lreg16(1) - 1; sreg16(1, c); if (c && !get_ZF(cpu)) JUMP_REL(d); }
	else { u32 c = lreg32(1) - 1; sreg32(1, c); if (c && !get_ZF(cpu)) JUMP_REL(d); }
	NEXT; }
op_jcxz: { s8 d = *pc++;
	if (adsz16 ? lreg16(1) == 0 : lreg32(1) == 0) JUMP_REL(d);
	NEXT; }
op_enter: {   // nesting level 0 only
	u16 size = *(u16a *) pc;
	if (pc[2] & 31) goto slow;
	pc += 3;
	if (opsz16) {
		u8 *s; STACK_PTR(s, -2, 2, 1);
		uword sp = REGi(4);
		set_sp(sp - 2 - size, sp_mask);
		fs16(s, lreg16(5));
		sreg16(5, (sp - 2) & sp_mask);
	} else {
		u8 *s; STACK_PTR(s, -4, 4, 1);
		uword sp = REGi(4);
		set_sp(sp - 4 - size, sp_mask);
		fs32(s, lreg32(5));
		sreg32(5, (sp - 4) & sp_mask);
	}
	NEXT; }
op_leave: {
	uword bp = REGi(5) & sp_mask;
	if (opsz16) {
		u8 *p; FPTR(p, SEG_SS, bp, 2, 0);
		set_sp(REGi(5) + 2, sp_mask);
		sreg16(5, fm16(p));
	} else {
		u8 *p; FPTR(p, SEG_SS, bp, 4, 0);
		set_sp(REGi(5) + 4, sp_mask);
		sreg32(5, fm32(p));
	}
	NEXT; }

	// far jumps, calls and returns, INT and IRET: in real mode (the rest goes through descriptors: tiny386)
#define LOAD_CS(sel) do { \
		cpu->seg[SEG_CS].sel = (sel); \
		cpu->seg[SEG_CS].base = (uword) (sel) << 4; \
		cpu->seg[SEG_CS].limit = 0xffff; \
		cpu->seg[SEG_CS].flags = 0; \
		cpu->cpl = cpu->flags & VM ? 3 : 0; \
		cpu->code16 = true; \
	} while (0)
op_jmpf: {
	uword off;
	if (opsz16) { off = *(u16a *) pc; pc += 2; } else { off = *(u32a *) pc; pc += 4; }
	u16 sel = *(u16a *) pc;
	if (PROTECTED) { pc += 2; PM_FAR(pmcall(cpu, opsz16, off, sel, true)); }
	LOAD_CS(sel);
	eip = off;
	RELOAD; }
op_callf: {
	uword off;
	if (opsz16) { off = *(u16a *) pc; pc += 2; } else { off = *(u32a *) pc; pc += 4; }
	u16 sel = *(u16a *) pc; pc += 2;
	if (PROTECTED) PM_FAR(pmcall(cpu, opsz16, off, sel, false));
	if (opsz16) {
		u8 *s; STACK_PTR(s, -4, 4, 1);
		SP_ADD(-4);
		fs16(s + 2, cpu->seg[SEG_CS].sel);
		fs16(s, EIP_OF(pc));
	} else {
		u8 *s; STACK_PTR(s, -8, 8, 1);
		SP_ADD(-8);
		fs32(s + 4, cpu->seg[SEG_CS].sel);
		fs32(s, EIP_OF(pc));
	}
	LOAD_CS(sel);
	eip = off;
	RELOAD; }
op_retf: {
	uword n = 0;
	if (pc[-1] == 0xca) { n = *(u16a *) pc; pc += 2; }
	if (PROTECTED) PM_FAR(pmret(cpu, opsz16, n, false));
	if (opsz16) {
		u8 *s; STACK_PTR(s, 0, 4, 0);
		eip = fm16(s);
		u16 sel = fm16(s + 2);
		LOAD_CS(sel);
		SP_ADD(4 + n);
	} else {
		u8 *s; STACK_PTR(s, 0, 8, 0);
		eip = fm32(s);
		u16 sel = fm32(s + 4);
		LOAD_CS(sel);
		SP_ADD(8 + n);
	}
	RELOAD; }
op_int: {   // INT n in real mode: push FLAGS, CS, IP; CS:IP from the vector table
	if (PMODE) goto slow;
	u8 n = *pc++;
	t386_int_vec[n]++;
#ifdef T386_INT_STATS
	t386_int_sw[n][(REGi(0) >> 8) & 0xff]++;
#endif
	u8 *v; uword vec = cpu->idt.base + n * 4;
	if ((vec & 0xfff) > 0xffc || vec >= 0xa0000) goto slow;
	v = cpu->phys_mem + vec;
	u8 *s; STACK_PTR(s, -6, 6, 1);
	refresh_flags(cpu);
	cpu->cc.mask = 0;
	fs16(s + 4, cpu->flags);
	fs16(s + 2, cpu->seg[SEG_CS].sel);
	fs16(s, EIP_OF(pc));
	SP_ADD(-6);
	eip = fm16(v);
	u16 sel = fm16(v + 2);
	LOAD_CS(sel);
	cpu->flags &= ~(IF | TF);
	RELOAD; }
op_iret: {   // in protected mode by tiny386's pmret; in real mode, 16-bit
	if (PROTECTED) PM_FAR(pmret(cpu, opsz16, 0, true));
	if (!opsz16 || (cpu->flags & VM)) goto slow;
	u8 *s; STACK_PTR(s, 0, 6, 0);
	eip = fm16(s);
	u16 sel = fm16(s + 2);
	cpu->flags = (cpu->flags & 0xffff0000) | fm16(s + 4);
	cpu->flags &= EFLAGS_MASK;
	cpu->flags |= 0x2;
	LOAD_CS(sel);
	cpu->cc.mask = 0;
	SP_ADD(6);
	if (cpu->intr && (cpu->flags & IF)) {   // as tiny386: the interrupt now
		cpu->next_ip = eip;
		goto out_ip;
	}
	RELOAD; }

	// groups
op_grp1_b: { u8 m = *pc++; int op = (m >> 3) & 7, rm = m & 7;
	if (m >= 0xc0) {
		u8 i = *pc++;
		switch (op) {
		case 0: { ADDb(rm, i, fr8, fw8, limm, 0) } break;
		case 1: { ORb(rm, i, fr8, fw8, limm, 0) } break;
		case 2: { ADCb(rm, i, fr8, fw8, limm, 0) } break;
		case 3: { SBBb(rm, i, fr8, fw8, limm, 0) } break;
		case 4: { ANDb(rm, i, fr8, fw8, limm, 0) } break;
		case 5: { SUBb(rm, i, fr8, fw8, limm, 0) } break;
		case 6: { XORb(rm, i, fr8, fw8, limm, 0) } break;
		default: { FCMPb(rm, i, fr8, fw8, limm, 0) } break;
		}
	} else {
		u8 *p; EA_PTR(p, m, 1, op != 7);
		u8 i = *pc++;
		switch (op) {
		case 0: { ADDb(p, i, fm8, fs8, limm, 0) } break;
		case 1: { ORb(p, i, fm8, fs8, limm, 0) } break;
		case 2: { ADCb(p, i, fm8, fs8, limm, 0) } break;
		case 3: { SBBb(p, i, fm8, fs8, limm, 0) } break;
		case 4: { ANDb(p, i, fm8, fs8, limm, 0) } break;
		case 5: { SUBb(p, i, fm8, fs8, limm, 0) } break;
		case 6: { XORb(p, i, fm8, fs8, limm, 0) } break;
		default: { FCMPb(p, i, fm8, fs8, limm, 0) } break;
		}
	}
	NEXT; }
#define GRP1_V(I16, I32) \
	if (m >= 0xc0) { \
		if (opsz16) { \
			u16 i = I16; \
			switch (op) { \
			case 0: { ADDw(rm, i, lreg16, sreg16, limm, 0) } break; \
			case 1: { ORw(rm, i, lreg16, sreg16, limm, 0) } break; \
			case 2: { ADCw(rm, i, lreg16, sreg16, limm, 0) } break; \
			case 3: { SBBw(rm, i, lreg16, sreg16, limm, 0) } break; \
			case 4: { ANDw(rm, i, lreg16, sreg16, limm, 0) } break; \
			case 5: { SUBw(rm, i, lreg16, sreg16, limm, 0) } break; \
			case 6: { XORw(rm, i, lreg16, sreg16, limm, 0) } break; \
			default: { FCMPw(rm, i, lreg16, sreg16, limm, 0) } break; \
			} \
		} else { \
			u32 i = I32; \
			switch (op) { \
			case 0: { ADDd(rm, i, lreg32, sreg32, limm, 0) } break; \
			case 1: { ORd(rm, i, lreg32, sreg32, limm, 0) } break; \
			case 2: { ADCd(rm, i, lreg32, sreg32, limm, 0) } break; \
			case 3: { SBBd(rm, i, lreg32, sreg32, limm, 0) } break; \
			case 4: { ANDd(rm, i, lreg32, sreg32, limm, 0) } break; \
			case 5: { SUBd(rm, i, lreg32, sreg32, limm, 0) } break; \
			case 6: { XORd(rm, i, lreg32, sreg32, limm, 0) } break; \
			default: { FCMPd(rm, i, lreg32, sreg32, limm, 0) } break; \
			} \
		} \
	} else if (opsz16) { \
		u8 *p; EA_PTR(p, m, 2, op != 7); \
		u16 i = I16; \
		switch (op) { \
		case 0: { ADDw(p, i, fm16, fs16, limm, 0) } break; \
		case 1: { ORw(p, i, fm16, fs16, limm, 0) } break; \
		case 2: { ADCw(p, i, fm16, fs16, limm, 0) } break; \
		case 3: { SBBw(p, i, fm16, fs16, limm, 0) } break; \
		case 4: { ANDw(p, i, fm16, fs16, limm, 0) } break; \
		case 5: { SUBw(p, i, fm16, fs16, limm, 0) } break; \
		case 6: { XORw(p, i, fm16, fs16, limm, 0) } break; \
		default: { FCMPw(p, i, fm16, fs16, limm, 0) } break; \
		} \
	} else { \
		u8 *p; EA_PTR(p, m, 4, op != 7); \
		u32 i = I32; \
		switch (op) { \
		case 0: { ADDd(p, i, fm32, fs32, limm, 0) } break; \
		case 1: { ORd(p, i, fm32, fs32, limm, 0) } break; \
		case 2: { ADCd(p, i, fm32, fs32, limm, 0) } break; \
		case 3: { SBBd(p, i, fm32, fs32, limm, 0) } break; \
		case 4: { ANDd(p, i, fm32, fs32, limm, 0) } break; \
		case 5: { SUBd(p, i, fm32, fs32, limm, 0) } break; \
		case 6: { XORd(p, i, fm32, fs32, limm, 0) } break; \
		default: { FCMPd(p, i, fm32, fs32, limm, 0) } break; \
		} \
	}
op_grp1_v: { u8 m = *pc++; int op = (m >> 3) & 7, rm = m & 7;
	GRP1_V((pc += 2, *(u16a *) (pc - 2)), (pc += 4, *(u32a *) (pc - 4)))
	NEXT; }
op_grp1_vb: { u8 m = *pc++; int op = (m >> 3) & 7, rm = m & 7;
	GRP1_V((u16) (s8) *pc++, (u32) (s8) *pc++)
	NEXT; }

	// shifts and rotates: C0 (Eb,Ib) C1 (Ev,Ib) D0 (Eb,1) D1 (Ev,1) D2 (Eb,CL) D3 (Ev,CL)
#define GRP2_CASES(SFX, X, N, LA, SA) \
		switch (op) { \
		case 0: { ROL##SFX(X, N, LA, SA, limm, 0) } break; \
		case 1: { ROR##SFX(X, N, LA, SA, limm, 0) } break; \
		case 2: { RCL##SFX(X, N, LA, SA, limm, 0) } break; \
		case 3: { RCR##SFX(X, N, LA, SA, limm, 0) } break; \
		case 4: case 6: { SHL##SFX(X, N, LA, SA, limm, 0) } break; \
		case 5: { SHR##SFX(X, N, LA, SA, limm, 0) } break; \
		default: { SAR##SFX(X, N, LA, SA, limm, 0) } break; \
		}
op_grp2_b: { u8 b = pc[-1]; u8 m = *pc++; int op = (m >> 3) & 7, rm = m & 7;
	if (m >= 0xc0) {
		u8 n = b == 0xc0 ? *pc++ : b == 0xd0 ? 1 : fr8(1);
		GRP2_CASES(b, rm, n, fr8, fw8)
	} else {
		u8 *p; EA_PTR(p, m, 1, 1);
		u8 n = b == 0xc0 ? *pc++ : b == 0xd0 ? 1 : fr8(1);
		GRP2_CASES(b, p, n, fm8, fs8)
	}
	NEXT; }
op_grp2_v: { u8 b = pc[-1]; u8 m = *pc++; int op = (m >> 3) & 7, rm = m & 7;
	if (m >= 0xc0) {
		u8 n = b == 0xc1 ? *pc++ : b == 0xd1 ? 1 : fr8(1);
		if (opsz16) { GRP2_CASES(w, rm, n, lreg16, sreg16) }
		else { GRP2_CASES(d, rm, n, lreg32, sreg32) }
	} else if (opsz16) {
		u8 *p; EA_PTR(p, m, 2, 1);
		u8 n = b == 0xc1 ? *pc++ : b == 0xd1 ? 1 : fr8(1);
		GRP2_CASES(w, p, n, fm16, fs16)
	} else {
		u8 *p; EA_PTR(p, m, 4, 1);
		u8 n = b == 0xc1 ? *pc++ : b == 0xd1 ? 1 : fr8(1);
		GRP2_CASES(d, p, n, fm32, fs32)
	}
	NEXT; }

	// F6/F7: TEST NOT NEG MUL IMUL DIV IDIV
#define GRP3(SFX, SZ, X, LA, SA, IMM) \
		switch (op) { \
		case 0: case 1: { u##SZ i = IMM; FTEST##SFX(X, i, LA, SA, limm, 0) } break; \
		case 2: { NOT##SFX(X, LA, SA) } break; \
		case 3: { NEG##SFX(X, LA, SA) } break; \
		case 4: { MUL##SFX(X, LA, SA) } break; \
		case 5: { IMUL##SFX(X, LA, SA) } break; \
		case 6: { cpu->ip = EIP_OF(ins); DIV##SFX(X, LA, SA) } break;   /* may raise #DE */ \
		default: { cpu->ip = EIP_OF(ins); IDIV##SFX(X, LA, SA) } break; \
		}
op_grp3_b: { u8 m = *pc++; int op = (m >> 3) & 7, rm = m & 7;
	if (m >= 0xc0) { GRP3(b, 8, rm, fr8, fw8, *pc++) }
	else { u8 *p; EA_PTR(p, m, 1, op == 2 || op == 3); GRP3(b, 8, p, fm8, fs8, *pc++) }
	NEXT; }
op_grp3_v: { u8 m = *pc++; int op = (m >> 3) & 7, rm = m & 7;
	if (m >= 0xc0) {
		if (opsz16) { GRP3(w, 16, rm, lreg16, sreg16, (pc += 2, *(u16a *) (pc - 2))) }
		else { GRP3(d, 32, rm, lreg32, sreg32, (pc += 4, *(u32a *) (pc - 4))) }
	} else if (opsz16) {
		u8 *p; EA_PTR(p, m, 2, op == 2 || op == 3);
		GRP3(w, 16, p, fm16, fs16, (pc += 2, *(u16a *) (pc - 2)))
	} else {
		u8 *p; EA_PTR(p, m, 4, op == 2 || op == 3);
		GRP3(d, 32, p, fm32, fs32, (pc += 4, *(u32a *) (pc - 4)))
	}
	NEXT; }

	// FE: INC/DEC Eb;  FF: INC/DEC Ev, CALL/JMP near Ev, PUSH Ev (far ones: tiny386)
op_grp4: { u8 m = *pc++; int op = (m >> 3) & 7, rm = m & 7;
	if (op > 1) goto slow;
	if (m >= 0xc0) {
		if (op == 0) { INCb(rm, fr8, fw8) } else { DECb(rm, fr8, fw8) }
	} else {
		u8 *p; EA_PTR(p, m, 1, 1);
		if (op == 0) { INCb(p, fm8, fs8) } else { DECb(p, fm8, fs8) }
	}
	NEXT; }
op_grp5: { u8 m = *pc++; int op = (m >> 3) & 7, rm = m & 7;
	if (op == 7 || ((op == 3 || op == 5) && m >= 0xc0)) goto slow;
	if (op == 3 || op == 5) {   // CALL / JMP far through memory: offset, then the selector
		u8 *p;
		uword off;
		u16 sel;
		if (opsz16) { EA_PTR(p, m, 4, 0); off = fm16(p); sel = fm16(p + 2); }
		else { EA_PTR(p, m, 6, 0); off = fm32(p); sel = fm16(p + 4); }
		if (PROTECTED) PM_FAR(pmcall(cpu, opsz16, off, sel, op == 5));
		if (op == 3) {
			if (opsz16) {
				u8 *s; STACK_PTR(s, -4, 4, 1);
				SP_ADD(-4);
				fs16(s + 2, cpu->seg[SEG_CS].sel);
				fs16(s, EIP_OF(pc));
			} else {
				u8 *s; STACK_PTR(s, -8, 8, 1);
				SP_ADD(-8);
				fs32(s + 4, cpu->seg[SEG_CS].sel);
				fs32(s, EIP_OF(pc));
			}
		}
		LOAD_CS(sel);
		eip = off;
		RELOAD;
	}
	uword v;
	u8 *p = NULL;
	if (m >= 0xc0) v = opsz16 ? lreg16(rm) : lreg32(rm);
	else if (opsz16) { EA_PTR(p, m, 2, op < 2); v = fm16(p); }
	else { EA_PTR(p, m, 4, op < 2); v = fm32(p); }
	switch (op) {
	case 0: case 1:
		if (p) {
			if (opsz16) { if (op == 0) { INCw(p, fm16, fs16) } else { DECw(p, fm16, fs16) } }
			else { if (op == 0) { INCd(p, fm32, fs32) } else { DECd(p, fm32, fs32) } }
		} else {
			if (opsz16) { if (op == 0) { INCw(rm, lreg16, sreg16) } else { DECw(rm, lreg16, sreg16) } }
			else { if (op == 0) { INCd(rm, lreg32, sreg32) } else { DECd(rm, lreg32, sreg32) } }
		}
		break;
	case 2:
		if (opsz16) { u8 *s; STACK_PTR(s, -2, 2, 1); SP_ADD(-2); fs16(s, EIP_OF(pc)); }
		else { u8 *s; STACK_PTR(s, -4, 4, 1); SP_ADD(-4); fs32(s, EIP_OF(pc)); }
		JUMP_ABS(v);
		break;
	case 4:
		JUMP_ABS(v);
		break;
	default:   // 6: PUSH
		if (opsz16) { u8 *s; STACK_PTR(s, -2, 2, 1); SP_ADD(-2); fs16(s, v); }
		else { u8 *s; STACK_PTR(s, -4, 4, 1); SP_ADD(-4); fs32(s, v); }
		break;
	}
	NEXT; }

	// string instructions; REP runs in chunks within a page, as tiny386
#define STR_SEG (SEG_OVERRIDE < 0 ? SEG_DS : SEG_OVERRIDE)
#define SI_ (adsz16 ? lreg16(6) : lreg32(6))
#define DI_ (adsz16 ? lreg16(7) : lreg32(7))
#define CX_ (adsz16 ? lreg16(1) : lreg32(1))
#define SET_SI(v) do { if (adsz16) sreg16(6, v); else sreg32(6, v); } while (0)
#define SET_DI(v) do { if (adsz16) sreg16(7, v); else sreg32(7, v); } while (0)
#define SET_CX(v) do { if (adsz16) sreg16(1, v); else sreg32(1, v); } while (0)
	// elements (of n bytes) the access at offset off can go through in its page, in direction dir
#define IN_PAGE(lin, n, dir) ((dir) > 0 ? (0x1000 - ((lin) & 0xfff)) / (n) : ((lin) & 0xfff) / (n) + 1)
	// cnt elements written from d on, in a page with translated code: all of them reported (ftlb_miss: the first)
#define CODE_RUN(d, cnt, n, dir) do { \
		uword _pa = (d) - cpu->phys_mem; \
		if (t386_pagemap && (t386_pagemap[_pa >> 12] & 3)) \
			t386_code_write((dir) > 0 ? _pa : _pa - ((cnt) - 1) * (n), (cnt) * (n)); \
	} while (0)
op_movsb: strn = 1; goto movs;
op_movs: strn = opsz16 ? 2 : 4;
movs: { int n = strn;
	int dir = (cpu->flags & DF) ? -n : n;
	int ss = STR_SEG;
	if (!rep) {
		u8 *s, *d;
		FPTR_IO(s, ss, SI_, n, 0, movs_io);
		FPTR_IO(d, SEG_ES, DI_, n, 1, movs_io);
		if (n == 1) fs8(d, fm8(s)); else if (n == 2) fs16(d, fm16(s)); else fs32(d, fm32(s));
		SET_SI(SI_ + dir);
		SET_DI(DI_ + dir);
		NEXT;
movs_io:   // either side may be MMIO; both are checked before anything is read (MMIO reads have side effects)
		{
			uword ps = 0, pd = 0;
			int ks = fast_where(cpu, cpu->seg[ss].base + SI_, n, 0, &s, &ps);
			int kd = fast_where(cpu, cpu->seg[SEG_ES].base + DI_, n, 1, &d, &pd);
			if (!ks || !kd) goto slow;
			u32 v = ks == 1 ? (n == 1 ? fm8(s) : n == 2 ? fm16(s) : fm32(s)) : fast_io_read(cpu, ps, n);
			if (kd == 2) fast_io_write(cpu, pd, n, v);
			else if (n == 1) fs8(d, v); else if (n == 2) fs16(d, v); else fs32(d, v);
			SET_SI(SI_ + dir);
			SET_DI(DI_ + dir);
			NEXT;
		}
	}
	while (CX_) {
		uword si = SI_, di = DI_;
		uword ls = cpu->seg[ss].base + si, ld = cpu->seg[SEG_ES].base + di;
		if ((ls | ld) & (n - 1)) goto slow;   // misaligned: tiny386 goes element by element
		uword cnt = CX_, c1 = IN_PAGE(ls, n, dir), c2 = IN_PAGE(ld, n, dir);
		if (c1 < cnt) cnt = c1;
		if (c2 < cnt) cnt = c2;
		if (adsz16 && (dir > 0 ? (si > di ? si : di) + cnt * n > 0x10000 : (si < di ? si : di) < (cnt - 1) * n))
			goto slow;   // the chunk would wrap around the segment: tiny386 does it its way
		u8 *s, *d;
		FPTR(s, ss, si, n, 0);
		FPTR(d, SEG_ES, di, n, 1);
		CODE_RUN(d, cnt, n, dir);
		if (dir > 0 && (d + cnt * n <= s || d >= s + cnt * n)) {
			memcpy(d, s, cnt * n);
		} else {
			for (uword i = 0; i < cnt; i++, s += dir, d += dir) {
				if (n == 1) fs8(d, fm8(s)); else if (n == 2) fs16(d, fm16(s)); else fs32(d, fm32(s));
			}
		}
		SET_SI(si + cnt * dir);
		SET_DI(di + cnt * dir);
		SET_CX(CX_ - cnt);
	}
	NEXT; }
op_stosb: strn = 1; goto stos;
op_stos: strn = opsz16 ? 2 : 4;
stos: { int n = strn;
	int dir = (cpu->flags & DF) ? -n : n;
	u32 v = REGi(0);
	if (!rep) {
		u8 *d; FPTR_IO(d, SEG_ES, DI_, n, 1, stos_io);
		if (n == 1) fs8(d, v); else if (n == 2) fs16(d, v); else fs32(d, v);
		SET_DI(DI_ + dir);
		NEXT;
stos_io: {
			uword pa = fast_mmio_addr(cpu, mlin, n, 1);
			if (pa == (uword) -1) goto slow;
			fast_io_write(cpu, pa, n, v);
			SET_DI(DI_ + dir);
			NEXT;
		}
	}
	while (CX_) {
		uword di = DI_;
		uword ld = cpu->seg[SEG_ES].base + di;
		if (ld & (n - 1)) goto slow;
		uword cnt = CX_, c2 = IN_PAGE(ld, n, dir);
		if (c2 < cnt) cnt = c2;
		if (adsz16 && (dir > 0 ? di + cnt * n > 0x10000 : di < (cnt - 1) * n))
			goto slow;
		u8 *d; FPTR_IO(d, SEG_ES, di, n, 1, stos_rep_io);
		CODE_RUN(d, cnt, n, dir);
		if (n == 1 && dir > 0) memset(d, v, cnt);
		else for (uword i = 0; i < cnt; i++, d += dir) {
			if (n == 1) fs8(d, v); else if (n == 2) fs16(d, v); else fs32(d, v);
		}
		goto stos_rep_next;
stos_rep_io: {   // MMIO (video memory: a game may clear its floor and ceiling so): the page's part, an element
		uword pa = fast_mmio_addr(cpu, mlin, n, 1);   // at a time, through fast_io_write (t386_vga_ext.h)
		if (pa == (uword) -1) goto slow;
		for (uword i = 0; i < cnt; i++) fast_io_write(cpu, pa + i * dir, n, v);
	}
stos_rep_next:
		SET_DI(di + cnt * dir);
		SET_CX(CX_ - cnt);
	}
	NEXT; }
op_lodsb: strn = 1; goto lods;
op_lods: strn = opsz16 ? 2 : 4;
lods: { int n = strn;
	if (rep) goto slow;
	int dir = (cpu->flags & DF) ? -n : n;
	u8 *s; FPTR_IO(s, STR_SEG, SI_, n, 0, lods_io);
	if (n == 1) fw8(0, fm8(s)); else if (n == 2) sreg16(0, fm16(s)); else sreg32(0, fm32(s));
	SET_SI(SI_ + dir);
	NEXT;
lods_io: {
		uword pa = fast_mmio_addr(cpu, mlin, n, 0);
		if (pa == (uword) -1) goto slow;
		u32 v = fast_io_read(cpu, pa, n);
		if (n == 1) fw8(0, v); else if (n == 2) sreg16(0, v); else sreg32(0, v);
		SET_SI(SI_ + dir);
		NEXT;
	}
	}
op_scasb: strn = 1; goto scas;
op_scas: strn = opsz16 ? 2 : 4;
scas: { int n = strn;
	int dir = (cpu->flags & DF) ? -n : n;
	do {
		if (rep && !CX_) break;
		u8 *d; FPTR(d, SEG_ES, DI_, n, 0);
		SET_DI(DI_ + dir);
		if (rep) SET_CX(CX_ - 1);
		if (n == 1) { u8 a = fr8(0), b = fm8(d); FCMPb(a, b, limm, 0, limm, 0) }
		else if (n == 2) { u16 a = lreg16(0), b = fm16(d); FCMPw(a, b, limm, 0, limm, 0) }
		else { u32 a = lreg32(0), b = fm32(d); FCMPd(a, b, limm, 0, limm, 0) }
		if (rep && ((cpu->cc.dst == 0) == (rep == 2))) break;
	} while (rep);
	NEXT; }
op_cmpsb: strn = 1; goto cmps;
op_cmps: strn = opsz16 ? 2 : 4;
cmps: { int n = strn;
	int dir = (cpu->flags & DF) ? -n : n;
	int ss = STR_SEG;
	do {
		if (rep && !CX_) break;
		u8 *s, *d;
		FPTR(s, ss, SI_, n, 0);
		FPTR(d, SEG_ES, DI_, n, 0);
		SET_SI(SI_ + dir);
		SET_DI(DI_ + dir);
		if (rep) SET_CX(CX_ - 1);
		if (n == 1) { u8 a = fm8(s), b = fm8(d); FCMPb(a, b, limm, 0, limm, 0) }
		else if (n == 2) { u16 a = fm16(s), b = fm16(d); FCMPw(a, b, limm, 0, limm, 0) }
		else { u32 a = fm32(s), b = fm32(d); FCMPd(a, b, limm, 0, limm, 0) }
		if (rep && ((cpu->cc.dst == 0) == (rep == 2))) break;
	} while (rep);
	NEXT; }

	// port I/O, where it needs no permission check (real mode, or CPL <= IOPL)
#define IO_ALLOWED (!PMODE || (!(cpu->flags & VM) && cpu->cpl <= get_IOPL(cpu)))
op_in_ib: op_in_dx: {
	if (!IO_ALLOWED) goto slow;
	u8 b = pc[-1];
	int port = b < 0xe8 ? *pc++ : lreg16(2);
	if (!(b & 1)) fw8(0, cpu->cb.io_read8(cpu->cb.io, port));
	else if (opsz16) sreg16(0, cpu->cb.io_read16(cpu->cb.io, port));
	else sreg32(0, cpu->cb.io_read32(cpu->cb.io, port));
	NEXT; }
op_out_ib: op_out_dx: {
	if (!IO_ALLOWED) goto slow;
	u8 b = pc[-1];
	int port = b < 0xe8 ? *pc++ : lreg16(2);
	if (!(b & 1)) cpu->cb.io_write8(cpu->cb.io, port, fr8(0));
	else if (opsz16) cpu->cb.io_write16(cpu->cb.io, port, lreg16(0));
	else cpu->cb.io_write32(cpu->cb.io, port, lreg32(0));
	NEXT; }

	// two-byte opcodes
op_imul_r: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7;
	if (m >= 0xc0) {
		if (opsz16) { IMUL2w(r, rm, lreg16, sreg16, lreg16, sreg16) }
		else { IMUL2d(r, rm, lreg32, sreg32, lreg32, sreg32) }
	} else if (opsz16) { u8 *p; EA_PTR(p, m, 2, 0); IMUL2w(r, p, lreg16, sreg16, fm16, fs16) }
	else { u8 *p; EA_PTR(p, m, 4, 0); IMUL2d(r, p, lreg32, sreg32, fm32, fs32) }
	NEXT; }
op_movzx_b: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7;
	u8 v;
	if (m >= 0xc0) v = fr8(rm); else { u8 *p; EA_PTR(p, m, 1, 0); v = fm8(p); }
	if (opsz16) sreg16(r, v); else sreg32(r, v);
	NEXT; }
op_movzx_w: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7;
	u16 v;
	if (m >= 0xc0) v = lreg16(rm); else { u8 *p; EA_PTR(p, m, 2, 0); v = fm16(p); }
	if (opsz16) sreg16(r, v); else sreg32(r, v);
	NEXT; }
op_movsx_b: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7;
	u8 v;
	if (m >= 0xc0) v = fr8(rm); else { u8 *p; EA_PTR(p, m, 1, 0); v = fm8(p); }
	if (opsz16) sreg16(r, sext8(v)); else sreg32(r, sext8(v));
	NEXT; }
op_movsx_w: { u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7;
	u16 v;
	if (m >= 0xc0) v = lreg16(rm); else { u8 *p; EA_PTR(p, m, 2, 0); v = fm16(p); }
	if (opsz16) sreg16(r, v); else sreg32(r, sext16(v));
	NEXT; }
op_bsf: op_bsr: { u8 b = pc[-1]; u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7;
	if (m >= 0xc0) {
		if (opsz16) { if (b == 0xbc) { BSFw(r, rm, lreg16, sreg16, lreg16, sreg16) } else { BSRw(r, rm, lreg16, sreg16, lreg16, sreg16) } }
		else { if (b == 0xbc) { BSFd(r, rm, lreg32, sreg32, lreg32, sreg32) } else { BSRd(r, rm, lreg32, sreg32, lreg32, sreg32) } }
	} else if (opsz16) {
		u8 *p; EA_PTR(p, m, 2, 0);
		if (b == 0xbc) { BSFw(r, p, lreg16, sreg16, fm16, fs16) } else { BSRw(r, p, lreg16, sreg16, fm16, fs16) }
	} else {
		u8 *p; EA_PTR(p, m, 4, 0);
		if (b == 0xbc) { BSFd(r, p, lreg32, sreg32, fm32, fs32) } else { BSRd(r, p, lreg32, sreg32, fm32, fs32) }
	}
	NEXT; }
	// BT BTS BTR BTC Ev,Gv (a register bit offset reaches beyond the operand in memory)
#define BT_OPS(A, X, Y, LA, SA, LB, SB) \
		switch (b) { \
		case 0xa3: { BT##A(X, Y, LA, SA, LB, SB) } break; \
		case 0xab: { BTS##A(X, Y, LA, SA, LB, SB) } break; \
		case 0xb3: { BTR##A(X, Y, LA, SA, LB, SB) } break; \
		default: { BTC##A(X, Y, LA, SA, LB, SB) } break; \
		}
op_bt: { u8 b = pc[-1]; u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7;
	if (m >= 0xc0) {
		if (opsz16) { BT_OPS(w, rm, r, lreg16, sreg16, lreg16, sreg16) }
		else { BT_OPS(d, rm, r, lreg32, sreg32, lreg32, sreg32) }
	} else {
		seg = SEG_OVERRIDE;
		uword off = fast_ea(cpu, &pc, m, adsz16, &seg);
		u8 *p;
		if (opsz16) {
			off += lreg16(r) / 16 * 2;
			FPTR(p, seg, off, 2, b != 0xa3);
			BT_OPS(w, p, r, fm16, fs16, lreg16, sreg16)
		} else {
			off += lreg32(r) / 32 * 4;
			FPTR(p, seg, off, 4, b != 0xa3);
			BT_OPS(d, p, r, fm32, fs32, lreg32, sreg32)
		}
	}
	NEXT; }
op_bt_ib: { u8 m = *pc++; int op = (m >> 3) & 7, rm = m & 7;   // 0F BA /4-7 Ev,Ib
	if (op < 4) goto slow;
	u8 b = op == 4 ? 0xa3 : op == 5 ? 0xab : op == 6 ? 0xb3 : 0xbb;
	if (m >= 0xc0) {
		u8 i = *pc++;
		if (opsz16) { BT_OPS(w, rm, i, lreg16, sreg16, limm, 0) }
		else { BT_OPS(d, rm, i, lreg32, sreg32, limm, 0) }
	} else if (opsz16) {
		u8 *p; EA_PTR(p, m, 2, op != 4);
		u8 i = *pc++;
		BT_OPS(w, p, i, fm16, fs16, limm, 0)
	} else {
		u8 *p; EA_PTR(p, m, 4, op != 4);
		u8 i = *pc++;
		BT_OPS(d, p, i, fm32, fs32, limm, 0)
	}
	NEXT; }
	// SHLD/SHRD Ev,Gv,Ib|CL
#define SHxD(OP) { u8 b = pc[-1]; u8 m = *pc++; int r = (m >> 3) & 7, rm = m & 7; \
	if (m >= 0xc0) { \
		u8 c = (b & 1) ? fr8(1) : *pc++; \
		if (opsz16) { OP##w(rm, r, c, lreg16, sreg16, lreg16, sreg16, limm, 0) } \
		else { OP##d(rm, r, c, lreg32, sreg32, lreg32, sreg32, limm, 0) } \
	} else if (opsz16) { \
		u8 *p; EA_PTR(p, m, 2, 1); \
		u8 c = (b & 1) ? fr8(1) : *pc++; \
		OP##w(p, r, c, fm16, fs16, lreg16, sreg16, limm, 0) \
	} else { \
		u8 *p; EA_PTR(p, m, 4, 1); \
		u8 c = (b & 1) ? fr8(1) : *pc++; \
		OP##d(p, r, c, fm32, fs32, lreg32, sreg32, limm, 0) \
	} \
	NEXT; }
op_shld: SHxD(SHLD)
op_shrd: SHxD(SHRD)

	// ---- leaving
out:	// stepcount done; pc is the next instruction
	cpu->next_ip = EIP_OF(pc);
out_ip:
	FAST_COUNT();
	return true;

slow:	// the current instruction goes to tiny386
	eip = EIP_OF(ins);
	{   // (counted by opcode: serial "fast ops")
		const u8 *p = ins;
		int k = 0;
		while (k < 14 && (p[k] == 0x66 || p[k] == 0x67 || p[k] == 0xf2 || p[k] == 0xf3 || p[k] == 0xf0 ||
				  p[k] == 0x26 || p[k] == 0x2e || p[k] == 0x36 || p[k] == 0x3e || p[k] == 0x64 || p[k] == 0x65))
			k++;
		fast_slow_ops[p[k] != 0x0f ? p[k] : p[k + 1] <= 1 ? 512 + 8 * p[k + 1] + ((p[k + 2] >> 3) & 7) : 256 + p[k + 1]]++;
	}
slow_eip:
	nslow++;
	cpu->next_ip = eip;
	cpu->ifetch.paddr = 0;
	{
		bool ok = cpu_exec1(cpu, 1);
		cpu->ifetch.paddr = 0;
		if (!ok) {
			FAST_COUNT();
			return false;
		}
		ftlb_check(cpu);   // (tiny386 may have emptied its TLB: ours follows)
	}
	if (cpu->halt || (cpu->flags & VM) != vm0 || (cpu->intr && (cpu->flags & IF)) || --steps < 0) {
		FAST_COUNT();
		return true;
	}
	eip = cpu->next_ip;
	goto reload;
#undef opsz16
#undef adsz16
#undef rep
}

#ifdef T386_JIT
#include "t386_jit.h"
#else
void t386_code_write(uword addr, int len) {}
void t386_ram_written(uword addr, int len) {}
#endif

// tiny386's reset: the translations go too (the BIOS is loaded again around it)
void cpui386_reset(CPUI386 *cpu)
{
#ifdef T386_JIT
	if (jit_ready) jit_flush();
#endif
	ftlb_cr0 = -1;
	tiny386_reset(cpu);
}

// tiny386's cpui386_step, through the fast path (and the translator)
void cpui386_step(CPUI386 *cpu, int stepcount)
{
	if ((cpu->flags & IF) && cpu->intr) {
		cpu->intr = false;
		cpu->halt = false;
		int no = cpu->cb.pic_read_irq(cpu->cb.pic);
		cpu->ip = cpu->next_ip;
		t386_int_hw[no & 0xff]++;
		TRY1(call_isr(cpu, no, false, 1));
	}

	if (cpu->halt) {
		usleep(1);
		return;
	}

#ifdef T386_JIT
	int ret = jit_exec(cpu, stepcount);
#else
	int ret = fast_exec(cpu, stepcount);
#endif
	cpu->ifetch.paddr = 0;
	if (!ret) {
		bool pusherr = false;
		switch (cpu->excno) {
		case EX_DF: case EX_TS: case EX_NP: case EX_SS: case EX_GP:
		case EX_PF:
			pusherr = true;
		}
		cpu->next_ip = cpu->ip;

		TRY1(call_isr(cpu, cpu->excno, pusherr, 1));
	}
}
