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
#ifndef T386_NO_PD
#define T386_FAST_PD   // pre-decoded blocks (t386_fast_pd.h)
#endif

// A run of instructions stops here: an interrupt is due, or the trap flag is set (single steps: cpui386_step, which
// runs one instruction and then raises the debug trap)
#define FAST_STOP_DUE(cpu) (((cpu)->intr && ((cpu)->flags & IF)) || ((cpu)->flags & TF))

unsigned t386_irq_seq;   // hardware interrupts taken (cpui386_step: a loop that only waits, t386_jit.h)

typedef u16 __attribute__((may_alias)) u16a;
typedef u32 __attribute__((may_alias)) u32a;

// Translated code (t386_jit.h): a byte per 4 KB page of the physical address space, nonzero bits 0-1 where writes
// must be checked (the page holds translated code); NULL without the translator.
u8 *t386_pagemap;
void t386_code_write(uword addr, int len);

// ---- the TLB: linear page -> host address of RAM, filled from tiny386's page translation
// Hardware interrupts taken and INT instructions run, by vector (serial "cpu"); and with T386_INT_STATS
// the INTs the fast path runs, by vector and AH.
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
// Before each TLB, its direct regions: runs of pages that all lie at the same distance from their linear addresses
// in RAM (a DOS extender's program under a DPMI host with paging: a 3D game tested has its 14 MB heap in one and its
// code, data and stack in another; a kernel's map of RAM), found in the page tables (ftlb_dir_poll). An access
// within one needs no entry: translated code checks its address against the region its block's maker expected it in
// (jt_ptlb: two loads, a subtraction and a branch) before it looks the page up. As an entry, a region holds what
// the page tables said when it was made, until the TLB is emptied (CR0, CR3, a task switch: ftlb_flush) or one of
// its pages is invalidated (INVLPG), when its pages are read again (ftlb_dir_check): the 386's rule for its own
// TLB. Its pages are present and accessed already, and those for writes writable and dirty, so there is nothing to
// set in the page tables; those for writes hold no translated code (jit_mark_code_page ends that when one gets
// some).
#define FTLB_DIRS 4
struct ftlb_dir {
	uword rlo, rlim;   // reads: linear addresses from rlo, rlim bytes (a page short of the run's end: any access
	uword wlo, wlim;   // that starts there lies in it whole); writes: a part of them; rlim, wlim 0: none
	uintptr_t add;     // at host address linear + add
	uword pad[3];      // (32 bytes)
};
static struct { struct ftlb_dir dir[FTLB_DIRS]; struct ftlb_entry e[FTLB_N]; } ftlb_tabs[2];
#define ftlb_both(k) (ftlb_tabs[k].e)
// (from a TLB's address, where translated code has it)
#define FTLB_DIR_OFF(i, f) ((int) offsetof(struct ftlb_dir, f) - (FTLB_DIRS - (i)) * (int) sizeof(struct ftlb_dir))
static struct ftlb_entry *ftlb = ftlb_tabs[0].e;   // the one in use (translated code loads it: jit_translate)
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
	return ftlb_both((cpu->cr0 & CR0_PG) && cpu->cpl == 3);
}

// ---- direct regions (struct ftlb_dir)
int t386_dir = 1;                 // serial "jit dir 0|1"
// (t386_dir_min, t386_dir_every: variables, so that a test with few pages can set them small and get regions)
int t386_dir_min = 64;            // pages: shorter runs are left to the TLB
int t386_dir_every = 4000000;     // instructions between two searches of the page tables, while they find more
#define FTLB_DIR_MIN ((uword) t386_dir_min)
#define FTLB_DIR_WMIN ((uword) (t386_dir_min + 3) / 4)   // ... and a run's part for writes
#define FTLB_DIR_EVERY ((u32) t386_dir_every)
#define FTLB_DIR_AGAIN (FTLB_DIR_EVERY * 16)   // ... at most, when they don't (runs grow as a program touches its memory)
#define FTLB_DIR_LARGE (FTLB_DIR_MIN * 8)      // pages: a new region this long is worth making the blocks again (jit_exec)
// n pages from linear page pg, each at physical page number its own + dpg; wn of them from wpg for writes
#define FTLB_DIR_PDES 8
struct ftlb_run {
	uword pg, n, dpg, wpg, wn;
	// The page directory's entries for it as they were when its pages were last read (ftlb_dir_holds; npde 0: too
	// many), and whether a page table of it has been written since (dirty: ftlb_dir_pt_written, told by the
	// translator, which watches those pages' writes: watched). Clean and with the same entries, it holds without
	// its pages being read again: 3800 of them for a 3D game, in PSRAM, a hundred times a second (its DPMI host
	// empties the TLB twice for every interrupt), was 4% of the time.
	u8 npde, dirty, watched;
	u8 checks, reads;   // of its last checks after flushes (up to FTLB_DIR_TRIAL), those that read its pages again
	u32 pde[FTLB_DIR_PDES];
};
// A region whose page tables are written between most flushes costs more than it saves (a big protected-mode system
// writes its own all the time: 2000 times its 1800 pages read again while it starts, 0.4 s): after FTLB_DIR_TRIAL
// checks with more than a quarter of them reading, it is dropped and its run left alone for FTLB_DIR_BAN instructions.
#define FTLB_DIR_TRIAL 64
#define FTLB_DIR_BAN 48000000u
static struct { uword pg, n, dpg; u32 at; } ftlb_dir_banned[4];
static unsigned ftlb_dir_nban;
static void (*ftlb_dir_rewatch)(void);   // the translator's: the regions changed, so the page tables to watch
static struct ftlb_run ftlb_dirs[2][FTLB_DIRS];
static u32 ftlb_dir_reads;        // times a region's pages were read again
static u32 ftlb_dir_wait;         // instructions until the next search: more after one that found nothing new
static bool ftlb_dir_new;         // a large region was just found
static u32 ftlb_dir_at;           // cpu->cycle when the page tables were last searched
static u32 ftlb_dir_stat[4];      // searches, checks after a flush, regions that held, regions dropped (serial "jit")

// Are the n bytes at physical address at in RAM? (Not at + n <= size: an entry of the page directory near the top
// of the address space, 0xfffff021, would pass, the sum being 0.)
static inline bool ftlb_dir_in_ram(uword at, uword n, uword size)
{
	return n <= size && at <= size - n;
}

// The longest runs (at most max, of FTLB_DIR_MIN pages or more) in linear pages [from, to) for TLB k (1: privilege
// level 3) that a direct region could be: present, accessed (the page directory's entry too), the user's for
// k = 1, in RAM that isn't a device's neighbour; and in each its longest part for writes: writable, dirty, without
// translated code. Returns how many.
static int ftlb_dir_scan(CPUI386 *cpu, int k, uword from, uword to, struct ftlb_run *out, int max)
{
	const u8 *const m = (const u8 *) cpu->phys_mem;
	const uword size = cpu->phys_mem_size, npages = size >> 12;
	const u32 need = k ? 0x25 : 0x21;   // P, A (U/S)
	struct ftlb_run c = { 0, 0, 0, 0, 0 };
	uword cw = 0, cwn = 0;   // the part for writes being counted
	int nout = 0;
	u32 pde = 0;
	const u8 *pt = NULL;
	for (uword pg = from;; pg++) {
		bool rok = false, wok = false;
		uword ppg = 0;
		if (pg < to) {
			if (pg == from || !(pg & 1023)) {
				const uword at = (cpu->cr3 & ~0xfffu) + (pg >> 10) * 4;
				pde = 0;
				if (ftlb_dir_in_ram(at, 4, size)) memcpy(&pde, m + at, 4);
				pt = (pde & need) == need && !(pde & 0x80) && ftlb_dir_in_ram(pde & ~0xfffu, 4096, size) ? m + (pde & ~0xfffu) : NULL;
			}
			if (pt) {
				u32 pte;
				memcpy(&pte, pt + (pg & 1023) * 4, 4);
				ppg = pte >> 12;
				rok = (pte & need) == need && ppg < npages && !(t386_pagemap[ppg] & ~3);
				wok = rok && (pde & 2) && (pte & 0x42) == 0x42 && !t386_pagemap[ppg];
			}
		}
		if (rok && c.n && ppg - pg == c.dpg) {
			c.n++;
		} else {
			if (c.n >= FTLB_DIR_MIN) {   // a run ended: among the longest?
				int at = nout;
				if (nout == max) {
					at = 0;
					for (int i = 1; i < max; i++) if (out[i].n < out[at].n) at = i;
					if (out[at].n >= c.n) at = -1;
				} else {
					nout++;
				}
				if (at >= 0) out[at] = c;
			}
			c.pg = pg, c.n = rok, c.dpg = ppg - pg, c.wpg = 0, c.wn = 0;
			cwn = 0;
		}
		if (wok && c.n) {
			if (!cwn) cw = pg;
			if (++cwn > c.wn) c.wpg = cw, c.wn = cwn;
		} else {
			cwn = 0;
		}
		if (pg >= to) break;
		if (!pt) pg |= 1023;   // nothing mapped by this entry of the page directory: on to the next
	}
	return nout;
}

// Slot i of TLB k as translated code reads it. The user's regions (k = 1) are the other levels' too, where those
// have none of their own: their pages are theirs to read and write as well.
static void ftlb_dir_set(CPUI386 *cpu, int k, int i)
{
	const struct ftlb_run *r = &ftlb_dirs[k][i];
	for (int t = k; t >= 0; t--) {
		if (t != k && ftlb_dirs[0][i].n) break;
		struct ftlb_dir *d = &ftlb_tabs[t].dir[i];
		d->rlim = d->wlim = 0;
		d->rlo = r->pg << 12;
		d->wlo = r->wpg << 12;
		d->add = (uintptr_t) cpu->phys_mem + ((uintptr_t) r->dpg << 12);
		d->rlim = r->n >= FTLB_DIR_MIN ? (r->n - 1) << 12 : 0;
		d->wlim = r->n >= FTLB_DIR_MIN && r->wn >= FTLB_DIR_WMIN ? (r->wn - 1) << 12 : 0;
	}
	if (!k && !r->n && ftlb_dirs[1][i].n) ftlb_dir_set(cpu, 1, i);   // (its own gone: the user's again)
}

// Does run u of TLB k hold as the page tables are now? Its pages' entries read again, in order: present, accessed,
// (the user's,) at the same distance. (What the RAM behind them is can't have changed, but for translated code:
// ftlb_dir_code_page.) Its part for writes ends if a page of it is no longer writable and dirty.
static bool ftlb_dir_holds(CPUI386 *cpu, int k, struct ftlb_run *u)
{
	const u8 *const m = (const u8 *) cpu->phys_mem;
	const uword size = cpu->phys_mem_size;
	const u32 need = k ? 0x25 : 0x21;
	bool wok = true;
	int c = 0;
	ftlb_dir_reads++;
	u->dirty = 0;
	u->npde = ((u->pg + u->n - 1) >> 10) - (u->pg >> 10) + 1 <= FTLB_DIR_PDES ? ((u->pg + u->n - 1) >> 10) - (u->pg >> 10) + 1 : 0;
	for (uword pg = u->pg, end = u->pg + u->n; pg < end;) {
		const uword at = (cpu->cr3 & ~0xfffu) + (pg >> 10) * 4;
		u32 pde = 0;
		if (ftlb_dir_in_ram(at, 4, size)) memcpy(&pde, m + at, 4);
		if ((pde & need) != need || (pde & 0x80) || !ftlb_dir_in_ram(pde & ~0xfffu, 4096, size)) return false;
		if (u->npde) u->pde[c++] = pde;
		const uword stop = (pg | 1023) + 1 < end ? (pg | 1023) + 1 : end;
		const u8 *pt = m + (pde & ~0xfffu) + (pg & 1023) * 4;
		const bool wpde = pde & 2;
		for (; pg < stop; pg++, pt += 4) {
			u32 pte;
			memcpy(&pte, pt, 4);
			if ((pte & need) != need || (pte >> 12) - pg != u->dpg) return false;
			if (pg - u->wpg < u->wn && (!wpde || (pte & 0x42) != 0x42)) wok = false;
		}
	}
	if (!wok) u->wn = 0;
	return true;
}

// ... the page directory's entries for it the same as then?
static bool ftlb_dir_pdes_same(CPUI386 *cpu, const struct ftlb_run *u)
{
	const u8 *const m = (const u8 *) cpu->phys_mem;
	const uword at = (cpu->cr3 & ~0xfffu) + (u->pg >> 10) * 4;
	return u->npde && ftlb_dir_in_ram(at, u->npde * 4, cpu->phys_mem_size) && !memcmp(m + at, u->pde, u->npde * 4);
}

// A write to physical page p, a page table of some region (the translator's t386_code_write): read again at the
// next flush
static void ftlb_dir_pt_written(uword p)
{
	for (int k = 0; k < 2 * FTLB_DIRS; k++) {
		struct ftlb_run *const u = &ftlb_dirs[k / FTLB_DIRS][k % FTLB_DIRS];
		for (int c = 0; u->n && c < u->npde; c++)
			if (u->pde[c] >> 12 == p) u->dirty = 1;
	}
}

// The TLB was emptied, or a region's page invalidated: each region's pages read again; one that no longer holds
// whole is dropped (and looked for again later), and its part for writes is what it is now. Without paging nothing
// reads them, and the write to CR0 that turns it on again comes here.
static void ftlb_dir_check(CPUI386 *cpu)
{
	if (t386_dir && !(cpu->cr0 & CR0_PG)) return;
	const bool on = t386_dir && t386_pagemap;
	bool changed = false;
	for (int k = 0; k < 2; k++)
		for (int i = 0; i < FTLB_DIRS; i++) {
			struct ftlb_run *const u = &ftlb_dirs[k][i];
			if (!u->n) continue;
			ftlb_dir_stat[1]++;
			if (u->checks == FTLB_DIR_TRIAL) u->checks = u->reads = 0;
			u->checks++;
			if (on && u->watched && !u->dirty && ftlb_dir_pdes_same(cpu, u)) continue;   // (as it was)
			const u8 checks = u->checks, reads = u->reads + 1;   // (ftlb_dir_holds notes the entries anew)
			const bool costly = checks == FTLB_DIR_TRIAL && reads > FTLB_DIR_TRIAL / 4;
			if (costly) {
				ftlb_dir_banned[ftlb_dir_nban % 4].pg = u->pg;
				ftlb_dir_banned[ftlb_dir_nban % 4].n = u->n;
				ftlb_dir_banned[ftlb_dir_nban % 4].dpg = u->dpg;
				ftlb_dir_banned[ftlb_dir_nban++ % 4].at = cpu->cycle | 1;
			}
			if (on && !costly && ftlb_dir_holds(cpu, k, u)) {
				u->checks = checks, u->reads = reads;
				ftlb_dir_stat[2]++;
			} else {
				ftlb_dir_stat[3]++;
				u->n = 0;
				ftlb_dir_wait = FTLB_DIR_EVERY;
			}
			changed = true;   // (its page tables may be others now)
			ftlb_dir_set(cpu, k, i);
		}
	if (changed && ftlb_dir_rewatch) ftlb_dir_rewatch();
}

// Before translated code runs with paging (jit_exec): the page tables searched for runs, now and then, for the
// privilege level it runs at. A slot whose run is found again, longer or not, keeps it (the blocks made for it
// stay right); the rest go to the free slots, the longest first.
static void ftlb_dir_poll(CPUI386 *cpu)
{
	if ((u32) cpu->cycle - ftlb_dir_at < ftlb_dir_wait) return;
	ftlb_dir_at = cpu->cycle;
	if (!t386_dir || !t386_pagemap || !(cpu->cr0 & CR0_PG)) return;
	bool news = false;
	const int k = cpu->cpl == 3;
	if (!k) {   // (the user's regions serve these levels too: none of their own beside them)
		bool own = false, users = false;
		for (int i = 0; i < FTLB_DIRS; i++) own |= ftlb_dirs[0][i].n != 0, users |= ftlb_dirs[1][i].n != 0;
		if (users && !own) return;
	}
	ftlb_dir_stat[0]++;
	struct ftlb_run found[FTLB_DIRS], *const have = ftlb_dirs[k];
	const int n = ftlb_dir_scan(cpu, k, 0, 1u << 20, found, FTLB_DIRS);
	for (int i = 0; i < FTLB_DIRS; i++) {   // those it has: as they are now
		if (!have[i].n) continue;
		int f = 0;
		while (f < n && !(found[f].n && found[f].dpg == have[i].dpg && found[f].pg <= have[i].pg &&
				  found[f].pg + found[f].n >= have[i].pg + have[i].n)) f++;
		if (f == n) continue;
		if (found[f].n != have[i].n) news = true;
		have[i] = found[f];
		found[f].n = 0;
		if (!ftlb_dir_holds(cpu, k, &have[i])) have[i].n = 0;   // (the page directory's entries noted)
		ftlb_dir_set(cpu, k, i);
	}
	for (int j = 0; j < n; j++)   // (not those found too costly a while ago)
		for (int b = 0; b < 4; b++)
			if (ftlb_dir_banned[b].at && (u32) cpu->cycle - ftlb_dir_banned[b].at < FTLB_DIR_BAN &&
			    found[j].dpg == ftlb_dir_banned[b].dpg && found[j].pg < ftlb_dir_banned[b].pg + ftlb_dir_banned[b].n &&
			    ftlb_dir_banned[b].pg < found[j].pg + found[j].n)
				found[j].n = 0;
	for (int i = 0; i < FTLB_DIRS; i++) {   // the free slots
		if (have[i].n) continue;
		int f = -1;
		for (int j = 0; j < n; j++) if (found[j].n && (f < 0 || found[j].n > found[f].n)) f = j;
		if (f < 0) break;
		have[i] = found[f];
		found[f].n = 0;
		news = true;
		if (!ftlb_dir_holds(cpu, k, &have[i])) have[i].n = 0;
		if (have[i].n >= FTLB_DIR_LARGE) ftlb_dir_new = true;
		ftlb_dir_set(cpu, k, i);
	}
	if (ftlb_dir_rewatch) ftlb_dir_rewatch();
	ftlb_dir_wait = news || !ftlb_dir_wait ? FTLB_DIR_EVERY : ftlb_dir_wait * 2 < FTLB_DIR_AGAIN ? ftlb_dir_wait * 2 : FTLB_DIR_AGAIN;
}

// Physical page p got translated code (jit_mark_code_page): no longer in a region's part for writes, nor in that of
// the user's region as the other levels have it (ftlb_dir_set: if that copy kept the part, a kernel's store
// to such a page of a program's would go to RAM unseen, the translated code made from it left as it was)
static void ftlb_dir_code_page(CPUI386 *cpu, uword p)
{
	for (int k = 0; k < 2; k++)
		for (int i = 0; i < FTLB_DIRS; i++) {
			struct ftlb_run *const u = &ftlb_dirs[k][i];
			if (u->n && u->wn && p - (u->wpg + u->dpg) < u->wn) {   // (the next check or search finds what is left)
				u->wn = 0;
				ftlb_dir_set(cpu, k, i);
			}
		}
}

static void ftlb_flush(CPUI386 *cpu)
{
	ftlb_flushes++;
	for (int k = 0; k < 2; k++)
		for (int i = 0; i < FTLB_N; i++)
			ftlb_both(k)[i].rtag = ftlb_both(k)[i].wtag = ftlb_both(k)[i].ctag = -1;
	ftlb_cr0 = cpu->cr0 & FTLB_CR0;
	ftlb_cr3 = cpu->cr3;
	ftlb_gen = t386_tlb_gen;
	ftlb = ftlb_for(cpu);
	ftlb_dir_check(cpu);
	if (ftlb_on_flush) ftlb_on_flush();
}

// INVLPG: the page's entries go, here and in tiny386's TLB. (tiny386 emptied its whole TLB for each, and so this
// one, and with it what the translator keeps by linear address, its jump cache and the entries from other pages:
// Linux runs an INVLPG for every page it copies on a write after a fork.) If the translator may have such an entry
// for code on that page (ftlb_on_invlpg: true), everything goes as before. Serial "jit invlpg 0": always.
int t386_invlpg_page = 1;
static u32 ftlb_invlpgs, ftlb_invlpg_all;   // INVLPGs with paging, and those that emptied everything (serial "jit")
static unsigned t386_tlb_page_gen;          // counts those that didn't (jit_after: the block leaves)
static bool (*ftlb_on_invlpg)(uword pg);
static void t386_invlpg(CPUI386 *cpu, uint32_t laddr)
{
	const uword pg = laddr >> 12;
	if (!(cpu->cr0 & CR0_PG)) {
		tlb_clear(cpu);
		return;
	}
	ftlb_invlpgs++;
	if (!t386_invlpg_page || (ftlb_on_invlpg && ftlb_on_invlpg(pg))) {
		ftlb_invlpg_all++;
		tlb_clear(cpu);
		return;
	}
	struct tlb_entry *ent = &cpu->tlb.tab[pg % tlb_size];
	if (ent->lpgno == pg) ent->lpgno = -1;
	cpu->ifetch.laddr = -1;
	cpu->ifetch.paddr = 0;
	for (int k = 0; k < 2; k++) {
		struct ftlb_entry *e = &ftlb_both(k)[pg & (FTLB_N - 1)];
		if (e->rtag == pg) e->rtag = -1;
		if (e->wtag == pg) e->wtag = -1;
		if (e->ctag == pg) e->ctag = -1;
	}
	t386_tlb_page_gen++;
	for (int k = 0; k < 2 * FTLB_DIRS; k++)   // a page of a direct region: its pages are read again
		if (pg - ftlb_dirs[k / FTLB_DIRS][k % FTLB_DIRS].pg < ftlb_dirs[k / FTLB_DIRS][k % FTLB_DIRS].n) {
			ftlb_dirs[k / FTLB_DIRS][k % FTLB_DIRS].dirty = 1;
			ftlb_dir_check(cpu);
			break;
		}
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

// The linear frame buffer's pages are mapped straight to video memory like RAM, reads at RAM speed. Writes too, once
// a page is armed: its first write after the renderer last took the marks comes through ftlb_miss, which marks the
// whole page for the screen (t386_vga_fw.h) and gives it a write entry; when the renderer takes the marks again
// (t386_vram_epoch), the armed pages are marked once more (written since, maybe) and lose their write entries
// (ftlb_lfb_poll, at each run of the fast path or the translator). (Before, each write came through ftlb_miss to mark
// its piece: a quarter of the time Linux's graphics console spent printing.) With all arming slots taken, a write
// marks its page alone, through an entry with ctag: the whole page, as the write may be the first element of a REP
// MOVS or STOS, which goes on to the page's end (up to 1.7.0 its piece: with 2 KB pieces the page's other half
// stayed as it was on the screen). An entry with ctag is thus not always RAM's: translated code checks
// (t386_jit.h, jit_emit_ctagw).
#include "t386_vga_fw.h"
#define FTLB_LFB_ARMED 256
static struct { struct ftlb_entry *e; uword pg; uint32_t off; } ftlb_lfb_armed[FTLB_LFB_ARMED];
static int ftlb_lfb_narmed;
static uint32_t ftlb_lfb_epoch;   // t386_vram_epoch when the first of them was armed

// A write at off (in the frame buffer) through entry e for linear page pg: arm the page (or just mark it)
static void ftlb_lfb_write(struct ftlb_entry *e, uword pg, uint32_t off)
{
	off &= ~0xfffu;
	if (ftlb_lfb_narmed == FTLB_LFB_ARMED) {
		e->ctag = pg;
		t386_vram_mark(off, 0x1000);
		return;
	}
	if (!ftlb_lfb_narmed)   // (read before the mark: the renderer taking it meanwhile is then seen)
		ftlb_lfb_epoch = __atomic_load_n(&t386_vram_epoch, __ATOMIC_ACQUIRE);
	t386_vram_mark(off, 0x1000);
	ftlb_lfb_armed[ftlb_lfb_narmed].e = e;
	ftlb_lfb_armed[ftlb_lfb_narmed].pg = pg;
	ftlb_lfb_armed[ftlb_lfb_narmed++].off = off;
	e->wtag = pg;
	e->ctag = -1;
}

static noinline void ftlb_lfb_disarm(void)
{
	for (int i = 0; i < ftlb_lfb_narmed; i++) {
		struct ftlb_entry *e = ftlb_lfb_armed[i].e;
		t386_vram_mark(ftlb_lfb_armed[i].off, 0x1000);
		if (e->wtag == ftlb_lfb_armed[i].pg)   // (not another page's entry by now)
			e->wtag = -1, e->ctag = ftlb_lfb_armed[i].pg;
	}
	ftlb_lfb_narmed = 0;
}

static inline void ftlb_lfb_poll(void)
{
	if (ftlb_lfb_narmed && __atomic_load_n(&t386_vram_epoch, __ATOMIC_ACQUIRE) != ftlb_lfb_epoch)
		ftlb_lfb_disarm();
}

// The host address of a size-byte access at linear address lin, filling the TLB; NULL if tiny386 must do the
// access: it is not to RAM (VGA memory and other MMIO; the linear frame buffer is, above), crosses a page, or faults
// (tiny386 raises the fault when it redoes the instruction).
static noinline u8 *ftlb_miss(CPUI386 *cpu, uword lin, int size, int write)
{
	if ((lin & 0xfff) > 0x1000 - size)
		return NULL;
	uword pg = lin >> 12;
	struct ftlb_entry *e = &ftlb[pg & (FTLB_N - 1)];
	if (write && e->ctag == pg && e->rtag == pg) {   // a page with translated code, written before
		u8 *host = (u8 *) (uintptr_t) (lin + e->delta);
		if ((uword) (host - t386_lfb_ram) < t386_lfb_size) {   // (the linear frame buffer)
			ftlb_lfb_write(e, pg, host - t386_lfb_ram);
			return host;
		}
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
	if (paddr - t386_lfb_base < t386_lfb_size) {
		uintptr_t delta = (uintptr_t) t386_lfb_ram + (paddr - t386_lfb_base) - (pg << 12);
		if (e->delta != delta || e->rtag != pg)
			e->wtag = e->ctag = -1;
		e->delta = delta;
		e->rtag = pg;
		if (write)
			ftlb_lfb_write(e, pg, paddr - t386_lfb_base);
		return (u8 *) (uintptr_t) (lin + delta);
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
	if (paddr - 0xa0000 < 0x20000 && t386_vga_read_fast(paddr - 0xa0000, size, &v)) return v;   // (A0000-BFFFF)
	return size == 1 ? cpu->cb.iomem_read8(cpu->cb.iomem, paddr) :
	       size == 2 ? cpu->cb.iomem_read16(cpu->cb.iomem, paddr) : cpu->cb.iomem_read32(cpu->cb.iomem, paddr);
}

static inline void fast_io_write(CPUI386 *cpu, uword paddr, int size, u32 v)
{
	if (paddr - 0xa0000 < 0x20000 && t386_vga_write_fast(paddr - 0xa0000, size, v)) return;
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
// (Returned in a struct: fast_exec's pc and window must not have their addresses taken, or they live in memory.)
struct fast_win { const u8 *pc, *lo; uword len; uintptr_t ipd; };
static noinline bool fast_window(CPUI386 *cpu, uword eip, struct fast_win *w)
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
		if ((uword) (host - cpu->phys_mem) >= (uword) cpu->phys_mem_size)
			return false;   // (code in the linear frame buffer: tiny386)
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
	w->pc = host;
	w->lo = start;
	w->len = size - 15;
	w->ipd = (uintptr_t) host - eip;
	return true;
}

#ifdef T386_FAST_PD
#include "t386_fast_pd.h"
#endif

// i386.c's #DE (DIV, IDIV) leaves fast_exec through fault, as the other exceptions
#pragma push_macro("THROW0")
#undef THROW0
#define THROW0(ex) do { cpu->excno = (ex); goto fault; } while (0)

// Run up to stepcount instructions, as tiny386's cpu_exec1: false on an exception (cpu->excno, cpu->ip at the
// instruction), for cpui386_step to deliver. (reorder-blocks-algorithm=simple: GCC's default moves the rarely taken
// paths to the end of this big function, out of a RISC-V branch's reach, and every check on the hot path becomes a
// taken branch over a jump; a game 13% faster on the Tab5 without it: less code, fewer instruction cache misses.
// The simple algorithm keeps that and still saves some taken branches: protected-mode code 3% faster than none.)
static bool T386_FAST_IRAM __attribute__((optimize("O3", "reorder-blocks-algorithm=simple"))) fast_exec(CPUI386 *cpu, int stepcount)
{
	// the handlers' copies (t386_fast_ops.h) and their dispatch tables
#define FL_CAT2(a, b) a##b
#define FL_CAT(a, b) FL_CAT2(a, b)
#define FL(x) FL_CAT(x, FAST_SFX)
#define FAST_SFX _g
#include "t386_fast_tab.h"
#undef FAST_SFX
#define FAST_SFX _32
#include "t386_fast_tab.h"
#undef FAST_SFX
#define FAST_SFX _16
#include "t386_fast_tab.h"
#undef FAST_SFX
#ifdef T386_FAST_PD
#define PD_LABEL(n) [PD_##n] = &&pd_##n,
	static const void *const pd_tab[PD_N] = { PD_LIST(PD_LABEL) };
#undef PD_LABEL
#endif

	// virtual-8086 mode as real mode, but for what it traps or checks (INT, IRET, I/O, and with IOPL < 3 the
	// interrupt flag's instructions): tiny386. A change of mode leaves (vm0).
	const uword vm0 = cpu->flags & VM;

	int steps = stepcount - 1;   // instructions left after the current one
	int nslow = 0;            // instructions passed to tiny386 (which counts its own)
	const u8 *pc;             // the next instruction byte (host address)
	const u8 *ins;            // the current instruction's first byte
	const u8 *win_lo;         // the code window: instructions may start in [win_lo, win_end)
	const u8 *win_end;
	uintptr_t ipd;            // eip = pc - ipd
	bool code16;
	const void *const *ops_cur;   // the dispatch table for the code's size (ops_32 or ops_16)
	uword sp_mask;
	unsigned nullsegs;
	// the current instruction's prefixes: bit 0 16-bit operands, bit 1 16-bit addresses, bits 2-3 REP (1) or REPNE
	// (2), bits 4-6 the segment override + 1; pfx0 without prefixes
	unsigned pfx, pfx0;
	// (the generic copy's; the fixed-size copies define their own around their handlers)
#define opsz16 (pfx & 1)
#define adsz16 ((pfx >> 1) & 1)
#define FAST_CODE16 code16
#define PFX0 pfx0
#define FAST_DTAB ops_cur
#define rep ((pfx >> 2) & 3)
#define SEG_OVERRIDE ((int) (pfx >> 4) - 1)
	int seg = -1;             // a memory operand's segment
	int strn;                 // string instructions: the element size
	uword eip;
	uword mlin;               // an MMIO access: its linear address, size, value (stores), register (loads)
	u32 mval;
	int msize, mreg;
#ifdef T386_FAST_PD
	// pre-decoded blocks (t386_fast_pd.h; not for the translator's single instructions): the uop running, its
	// block, the block's code, where the last jump keeps its block
	const struct pd_uop *pu;
	struct pd_block *bk, **lk;
	const u8 *bh;
	bool pd_call = false;   // in pd_classic: an instruction run by the fast path's handlers, pd_saved steps left
	int pd_saved = 0;
	pd_h = pd_tab;
	// a block checks the steps left at its end: up to PD_MAX - 1 instructions more than asked (cpu->cycle counts
	// them), so blocks only for more steps than that; fewer (single steps) are exact
	const bool pd_on = t386_pd_enabled && stepcount > PD_MAX && pd_init(cpu);
	pd_ep++, pd_stat_ep[3]++;   // (devices may have written code since the last time)
	// (back from pd_classic: the steps it had; and whether blocks may go on, as the fast path would)
#define PD_UNCALL do { if (pd_call) { pd_call = false; steps += pd_saved; } } while (0)
#define PD_GO_ON (!cpu->halt && (cpu->flags & VM) == vm0 && !FAST_STOP_DUE(cpu))
#define PD_ENTER do { if (pd_on) goto pd_enter; } while (0)
#define PD_JUMPED do { if (pd_on) goto pd_jumped; } while (0)
#else
#define PD_ENTER do { } while (0)
#define PD_JUMPED do { } while (0)
#define PD_UNCALL do { } while (0)
#endif

#define EIP_OF(p) ((uword) ((uintptr_t) (p) - ipd))
	// every handler ends dispatching the next instruction itself (so that the host's branch predictor sees
	// one indirect jump per handler)
#define DISPATCH do { ins = pc; pfx = PFX0; \
		goto *FAST_DTAB[*pc++]; } while (0)
#define NEXT do { \
		if (unlikely(--steps < 0)) goto out; \
		if (unlikely((uintptr_t) pc >= (uintptr_t) win_end)) goto refill; \
		DISPATCH; \
	} while (0)
	// relative and absolute jumps; 16-bit code wraps at 64 KB. (NEXT checks only the window's end: a jump to below
	// its start moves the end there, so that NEXT finds the next instruction outside.)
#define JUMP_BELOW do { if (unlikely((uintptr_t) pc < (uintptr_t) win_lo)) win_end = pc; } while (0)
#define JUMP_REL(d) do { \
		if (FAST_CODE16) pc = (const u8 *) (ipd + ((EIP_OF(pc) + (d)) & 0xffff)); \
		else pc += (d); \
		PD_JUMPED; \
		JUMP_BELOW; \
	} while (0)
#define JUMP_ABS(neweip) do { pc = (const u8 *) (ipd + (uword) (neweip)); PD_JUMPED; JUMP_BELOW; } while (0)
	// a memory operand's host address, or the instruction goes to tiny386 (the entry's tag is compared with the page
	// of the last byte: an access across pages never matches, and ftlb_miss hands it over)
#define FPTR(p, sg, off, size, wr) do { \
		if (unlikely(nullsegs) && (nullsegs & (1 << (sg)))) goto slow; \
		uword _lin = cpu->seg[sg].base + (off); \
		uword _pg = _lin >> 12; \
		struct ftlb_entry *_e = &ftlb[_pg & (FTLB_N - 1)]; \
		if (likely(((wr) ? _e->wtag : _e->rtag) == ((_lin + (size) - 1) >> 12))) \
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
		if (likely(((wr) ? _e->wtag : _e->rtag) == ((_lin + (size) - 1) >> 12))) \
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
#define FAST_COUNT() do { PD_UNCALL; int _n = stepcount - 1 - steps - nslow; if (_n > 0) cpu->cycle += _n; } while (0)
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
		if (!(call)) goto fault; \
		ftlb_check(cpu); \
		eip = cpu->next_ip; \
		if ((cpu->flags & VM) != vm0 || FAST_STOP_DUE(cpu)) { goto out_ip; } \
		RELOAD; \
	} while (0)

	// the code window around eip (pc, win_lo, win_end, ipd), or the instruction goes to tiny386
#define WINDOW(eip) do { \
		struct fast_win _w; \
		if (!fast_window(cpu, (eip), &_w)) goto slow_eip; \
		pc = _w.pc; win_lo = _w.lo; win_end = _w.lo + _w.len; ipd = _w.ipd; \
	} while (0)

	eip = cpu->next_ip;
	cpu->ifetch.paddr = 0;
	ftlb_check(cpu);
	ftlb_lfb_poll();
reload:   // after a change of mode, CS or segment registers made outside the handlers below; eip is the next instruction
	code16 = cpu->code16;
	pfx0 = code16 ? 3 : 0;
	ops_cur = code16 ? ops_16 : ops_32;
	sp_mask = cpu->sp_mask;
	nullsegs = fast_nullsegs(cpu);
	WINDOW(eip);
	PD_ENTER;
	DISPATCH;
refill:   // the next instruction is outside the code window
	eip = EIP_OF(pc);
	WINDOW(eip);
	PD_ENTER;
	DISPATCH;

#define FAST_SFX _g
#include "t386_fast_ops.h"
#undef FAST_SFX
#undef opsz16
#undef adsz16
#undef FAST_CODE16
#undef PFX0
#undef FAST_DTAB
#define opsz16 0
#define adsz16 0
#define FAST_CODE16 0
#define PFX0 0u
#define FAST_DTAB ops_32
#define FAST_SFX _32
#include "t386_fast_ops.h"
#undef FAST_SFX
#undef opsz16
#undef adsz16
#undef FAST_CODE16
#undef PFX0
#undef FAST_DTAB
#define opsz16 1
#define adsz16 1
#define FAST_CODE16 1
#define PFX0 3u
#define FAST_DTAB ops_16
#define FAST_SFX _16
#include "t386_fast_ops.h"
#undef FAST_SFX
#undef opsz16
#undef adsz16
#undef FAST_CODE16
#undef PFX0
#undef FAST_DTAB
#define opsz16 (pfx & 1)
#define adsz16 ((pfx >> 1) & 1)
#define FAST_CODE16 code16
#define PFX0 pfx0
#define FAST_DTAB ops_cur

#ifdef T386_FAST_PD
#include "t386_fast_pd_ops.h"
#endif

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
	// ---- leaving
out:	// stepcount done; pc is the next instruction
#ifdef T386_FAST_PD
	if (pd_call) {   // (pd_classic's instruction done: on in its block if it went on to the next one)
		PD_UNCALL;
		if (PD_GO_ON) {
			if (pc == bh + pu[1].ip) {
				pu++;
				goto *pu->h;
			}
			goto pd_go;
		}
	}
#endif
	cpu->next_ip = EIP_OF(pc);
out_ip:
#ifdef T386_FAST_PD
	if (pd_call) {   // (pd_classic's instruction changed CS, or the like: cpu->next_ip is the next one)
		PD_UNCALL;
		if (steps >= 0 && PD_GO_ON) {
			eip = cpu->next_ip;
			goto reload;
		}
	}
#endif
	FAST_COUNT();
	return true;

fault:	// an exception at the current instruction (cpu->excno): counted with it, as cpu_exec1 counts
	FAST_COUNT();
	cpu->cycle++;
	return false;

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
	// cpu_exec1 counted it: into nslow only with its step, or FAST_COUNT leaves it out twice when stopping here
	if (cpu->halt || (cpu->flags & VM) != vm0 || FAST_STOP_DUE(cpu) || (nslow++, --steps < 0)) {
#ifdef T386_FAST_PD
		if (pd_call) {   // (pd_classic's instruction, by tiny386)
			PD_UNCALL;
			if (steps >= 0 && PD_GO_ON) {
				eip = cpu->next_ip;
				goto reload;
			}
		}
#endif
		FAST_COUNT();
		return true;
	}
	eip = cpu->next_ip;
	goto reload;
#undef opsz16
#undef adsz16
#undef rep
#undef FAST_CODE16
#undef PFX0
#undef FAST_DTAB
#undef FL
#undef PD_ENTER
#undef PD_JUMPED
#undef PD_UNCALL
#undef PD_GO_ON
}
#pragma pop_macro("THROW0")

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

// An interrupt or exception that can't be delivered (a fault while delivering it): a double fault (vector 8, error
// code 0), and if that can't be delivered either, a shutdown, which a PC's board answers with a reset: t386_shutdown,
// set by the firmware (the PC's reset_request). tiny386 aborts the program there (TRY1: cpu_abort), which on the Tab5
// was a panic: a program that had gone wild took the Tab5 down with it.
void (*t386_shutdown)(void);
static void fast_deliver(CPUI386 *cpu, int no, bool pusherr)
{
	if (call_isr(cpu, no, pusherr, 1))
		return;
	if (no != EX_DF) {
		cpu->excerr = 0;
		if (call_isr(cpu, EX_DF, true, 1))
			return;
	}
	if (!t386_shutdown)
		cpu_abort(cpu, -1);
	t386_shutdown();
}

// A step with the CPU halted (HLT, waiting for an interrupt): the platform's wait before the devices are looked at
// again. This one, a microsecond; the firmware's sleeps when no interrupt can be due soon (core 1 idles).
__attribute__((weak)) void t386_halt_wait(void) { usleep(1); }

// tiny386's cpui386_step, through the fast path (and the translator)
#ifdef ESP_PLATFORM
extern int t386_step_max, t386_step_us;   // (t386_pc.c: the longest step, and how long it is to last)
#endif
void cpui386_step(CPUI386 *cpu, int stepcount)
{
	if ((cpu->flags & IF) && cpu->intr) {
		cpu->intr = false;
		cpu->halt = false;
		int no = cpu->cb.pic_read_irq(cpu->cb.pic);
		cpu->ip = cpu->next_ip;
		t386_int_hw[no & 0xff]++;
		t386_irq_seq++;
		fast_deliver(cpu, no, false);
	}

	if (cpu->halt) {
		t386_halt_wait();
		return;
	}

#ifdef ESP_PLATFORM
	uint32_t c0, c1;
	asm volatile("csrr %0, mcycle" : "=r"(c0));
	const uint32_t i0 = (uint32_t) cpu->cycle;
#endif
	// the trap flag (a debugger's single steps): one instruction, by the fast path or tiny386, then the debug trap
	const bool tf = cpu->flags & TF;
#ifdef T386_JIT
	int ret = tf ? fast_exec(cpu, 1) : jit_exec(cpu, stepcount);
#else
	int ret = fast_exec(cpu, tf ? 1 : stepcount);
#endif
#ifdef ESP_PLATFORM
	asm volatile("csrr %0, mcycle" : "=r"(c1));
	// steps by time: the instructions that would have taken t386_step_us at this step's speed (360 cycles a
	// microsecond), 512 to 32768. t386_step_max drops to that at once when the guest got slower (one long step, not
	// a run of them: 32768 instructions of the slowest code take some 30 ms), and rises to it a quarter of the way
	// a step. A step cut short at once says little.
	if (t386_step_us) {
		const uint32_t ran = (uint32_t) cpu->cycle - i0, cyc = c1 - c0;
		if (ran >= 256 && cyc) {
			uint64_t want = (uint64_t) ran * (uint32_t) (t386_step_us * 360) / cyc;
			if (want < 512) want = 512;
			if (want > 32768) want = 32768;
			t386_step_max = (int) want < t386_step_max ? (int) want : (t386_step_max * 3 + (int) want + 3) / 4;
		}
	}
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
		

		fast_deliver(cpu, cpu->excno, pusherr);
	} else if (tf) {   // a trap after the instruction, as QEMU's: vector 1, DR6's BS set, the next instruction pushed
		cpu->dr[6] |= 0x4000;
		cpu->ip = cpu->next_ip;
		fast_deliver(cpu, EX_DB, false);
	}
}

// Physical memory moved (the VGA's BAR: the linear frame buffer elsewhere, or off): every translation goes, as at a
// TLB flush (t386_pc.c's BAR callback; translated code running leaves: jit_after)
void t386_tlb_forget(void) { t386_tlb_gen++; }
