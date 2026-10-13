/*
 * Host-side test of the sequencer in modsrc/tb330fu_ovl/tb330fu_ovl.c: the module source is #included with a tiny shim of the kernel API
 * and the display registers are plain memory.  The registers are initialised the way the CN LK programs them (ddp_ovl.c, disassembled):
 *   L_CON has 0x600 (both flips), ADDR is the END address  addr + pitch*(h-1) + 4*w + 3,  PITCH / SRC_SIZE plain.
 * Every step of the experiment sequence is driven by hand and the register state is checked.
 *
 * build: gcc -O1 -Wall -Wno-unused-function -Wno-pointer-arith -Ifakeinc -o hosttest 321_hosttest_ovl.c
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>

typedef uint32_t u32;
#define __iomem
#define __init
#define __exit
#define pr_info(...) printf(__VA_ARGS__)
#define pr_err(...) printf(__VA_ARGS__)
#define module_param(a, b, c)
#define MODULE_PARM_DESC(a, b)
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define module_init(x)
#define module_exit(x)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define wmb()
#define msecs_to_jiffies(x) (x)

struct work_struct { int unused; };
struct delayed_work { struct work_struct work; void (*fn)(struct work_struct *); };
#define INIT_DELAYED_WORK(w, f) ((w)->fn = (f))
static unsigned int last_delay_ms;
#define schedule_delayed_work(w, d) ((void)((last_delay_ms = (d)), (w)))
#define cancel_delayed_work_sync(w) ((void)0)

/* fake MMIO: every ioremap()ed physical page is a zeroed host buffer, remembered by physical address */
#define MAXMAP 16
static struct { unsigned long pa; uint8_t *buf; } maps[MAXMAP];
static int nmaps;
static int nwrites;
static void *ioremap(unsigned long pa, size_t sz)
{
	maps[nmaps].pa = pa;
	maps[nmaps].buf = calloc(1, sz);
	return maps[nmaps++].buf;
}
static void iounmap(void *p) { (void)p; }
static u32 readl_relaxed(const void *p) { return *(const volatile u32 *)p; }
static u32 readl(const void *p) { return *(const volatile u32 *)p; }
static void writel(u32 v, void *p)
{
	*(volatile u32 *)p = v;
	nwrites++;
}

#include "../tb330fu_ovl.c"

static uint8_t *page(unsigned long pa)
{
	for (int i = 0; i < nmaps; i++)
		if (maps[i].pa == pa)
			return maps[i].buf;
	return NULL;
}
static void put(unsigned long pa, unsigned off, u32 v) { *(u32 *)(page(pa) + off) = v; }
static u32 get(unsigned long pa, unsigned off) { return *(u32 *)(page(pa) + off); }

static int fails;
#define CHECK(name, cond) do { printf("%s  %s\n", (cond) ? "PASS" : "FAIL", name); if (!(cond)) fails++; } while (0)

static const unsigned long OVL0 = 0x1400b000UL, OVL2L = 0x1400c000UL;
#define SZ ((1920u << 16) | 1200u)

static void reset(void)
{
	for (int i = 0; i < nmaps; i++)
		memset(maps[i].buf, 0, 0x1000);
	nwrites = 0;
	step = 0;
	nc = 0;
}
/* program a layer the way LK does: flips, end address */
static void lk_layer(unsigned long pa, unsigned n, u32 base, u32 pitch)
{
	put(pa, OVL_ADDR(n), base + pitch * 1919 + 4 * 1200 + 3);
	put(pa, OVL_PITCH(n), pitch);
	put(pa, OVL_SRC_SIZE(n), SZ);
	put(pa, OVL_CON(n), 0x2000 | CON_FLIPS | 0x100 | 0xff | (n ? 0x1000000 : 0));
	put(pa, OVL_RDMA_CTRL(n), 1);
	put(pa, OVL_SRC_CON, get(pa, OVL_SRC_CON) | (1u << n));
}
static void run_step(void) { seq_fn(NULL); }

int main(void)
{
	if (ovl_init())
		return 2;
	printf("---- ovl_init done (%d blocks mapped)\n", nmaps);
	CHECK("5 blocks mapped", nmaps == 5);
	CHECK("lk_addr(buf0) = 0x7d1c7fc3 (what LK writes for layer0 on buffer 0)", lk_addr(0x7c8e0000) == 0x7d1c7fc3);
	CHECK("LK layer1 formula (pitch 4800, buffer 1) = 0x7da92003", 0x7d1c8000u + 4800u * 1919 + 4800 + 3 == 0x7da92003u);

	printf("\n==== scenario A: LK as logged: L0 buf0/4864 + L1 buf1/4800 on OVL0\n");
	reset();
	put(OVL0, OVL_EN, 1);
	put(OVL0, 0x028, 0xff000000);
	lk_layer(OVL0, 0, 0x7c8e0000, 4864);
	lk_layer(OVL0, 1, 0x7d1c8000, 4800);
	const u32 con0 = get(OVL0, OVL_CON(0)), con1 = get(OVL0, OVL_CON(1));
	nwrites = 0;
	run_step();	/* step 0: P1 */
	CHECK("A/P1: L1 disabled, L0 enabled", get(OVL0, OVL_SRC_CON) == 0x1);
	CHECK("A/P1: L0 addr = LK formula for fb_base (unchanged for buffer 0)", get(OVL0, OVL_ADDR(0)) == lk_addr(0x7c8e0000));
	CHECK("A/P1: L0 con untouched (flips stay)", get(OVL0, OVL_CON(0)) == con0 && (con0 & CON_FLIPS) == CON_FLIPS);
	CHECK("A/P1: L1 registers untouched except the enable bit", get(OVL0, OVL_ADDR(1)) == 0x7d1c8000u + 4800u * 1919 + 4803 && get(OVL0, OVL_PITCH(1)) == 4800);
	CHECK("A/P1: next step scheduled after hold_ms", last_delay_ms == hold_ms);
	run_step();	/* step 1: restore */
	CHECK("A/restore: SRC_CON back to 3", get(OVL0, OVL_SRC_CON) == 0x3);
	CHECK("A/restore: L0 and L1 as before", get(OVL0, OVL_ADDR(0)) == lk_addr(0x7c8e0000) && get(OVL0, OVL_CON(1)) == con1 && get(OVL0, OVL_PITCH(1)) == 4800);
	CHECK("A/restore: next step after gap_ms", last_delay_ms == gap_ms);
	run_step();	/* step 2: P4 */
	CHECK("A/P4: flips cleared on the keeper", (get(OVL0, OVL_CON(0)) & CON_FLIPS) == 0 && (get(OVL0, OVL_CON(0)) | CON_FLIPS) == con0);
	CHECK("A/P4: plain start address", get(OVL0, OVL_ADDR(0)) == 0x7c8e0000);
	CHECK("A/P4: L1 disabled", get(OVL0, OVL_SRC_CON) == 0x1);
	run_step();	/* step 3: restore */
	CHECK("A/restore2: everything as LK left it", get(OVL0, OVL_CON(0)) == con0 && get(OVL0, OVL_ADDR(0)) == lk_addr(0x7c8e0000) && get(OVL0, OVL_SRC_CON) == 3);
	run_step();	/* step 4: final P1 */
	CHECK("A/final: P1 kept (L1 off, L0 LK-addressed)", get(OVL0, OVL_SRC_CON) == 1 && get(OVL0, OVL_ADDR(0)) == lk_addr(0x7c8e0000) && (get(OVL0, OVL_CON(0)) & CON_FLIPS) == CON_FLIPS);
	CHECK("A: ROI/bg registers never touched", get(OVL0, 0x028) == 0xff000000);

	printf("\n==== scenario B: layer 0 is on buffer 2 (second double-buffer phase)\n");
	reset();
	put(OVL0, OVL_EN, 1);
	lk_layer(OVL0, 0, 0x7dab0000, 4864);
	lk_layer(OVL0, 1, 0x7d1c8000, 4800);
	run_step();
	CHECK("B/P1: L0 re-aimed at buffer 0 with LK's formula", get(OVL0, OVL_ADDR(0)) == 0x7d1c7fc3 && get(OVL0, OVL_SRC_CON) == 1);
	run_step();
	CHECK("B/restore: back on buffer 2", get(OVL0, OVL_ADDR(0)) == 0x7dab0000u + 4864u * 1919 + 4803 && get(OVL0, OVL_SRC_CON) == 3);

	printf("\n==== scenario C: nothing reads the framebuffer region -> zero writes\n");
	reset();
	put(OVL0, OVL_EN, 1);
	lk_layer(OVL0, 0, 0x7aff8000, 4864);
	nwrites = 0;
	run_step();
	CHECK("C: zero writes, sequence stops", nwrites == 0 && nc == 0 && step == 0);

	printf("\n==== scenario D: only the pitch-4800 layer is enabled -> it becomes the keeper and gets pitch 4864\n");
	reset();
	put(OVL0, OVL_EN, 1);
	lk_layer(OVL0, 1, 0x7d1c8000, 4800);
	run_step();
	CHECK("D/P1: L1 kept with pitch 4864, size ok, addr = LK formula for fb_base", get(OVL0, OVL_SRC_CON) == 2 && (get(OVL0, OVL_PITCH(1)) & 0xffff) == 4864 &&
	      get(OVL0, OVL_ADDR(1)) == lk_addr(0x7c8e0000) && get(OVL0, OVL_SRC_SIZE(1)) == SZ);

	printf("\n==== scenario E: layers on OVL0_2L, nothing on OVL0\n");
	reset();
	put(OVL2L, OVL_EN, 1);
	lk_layer(OVL2L, 0, 0x7c8e0000, 4864);
	lk_layer(OVL2L, 1, 0x7d1c8000, 4800);
	run_step();
	CHECK("E/P1: OVL0_2L L1 disabled", get(OVL2L, OVL_SRC_CON) == 1 && get(OVL0, OVL_SRC_CON) == 0);
	run_step();
	CHECK("E/restore", get(OVL2L, OVL_SRC_CON) == 3);

	printf("\n==== scenario F: final=0 keeps the original state\n");
	reset();
	final = 0;
	put(OVL0, OVL_EN, 1);
	lk_layer(OVL0, 0, 0x7c8e0000, 4864);
	lk_layer(OVL0, 1, 0x7d1c8000, 4800);
	for (int i = 0; i < 5; i++)
		run_step();
	CHECK("F: after the sequence the registers equal LK's", get(OVL0, OVL_SRC_CON) == 3 && get(OVL0, OVL_ADDR(0)) == lk_addr(0x7c8e0000));
	final = 1;

	printf("\n==== scenario G: seq=0 writes nothing at all (init only reads)\n");
	nwrites = 0;
	{
		int s = seq;
		seq = 0;
		dump_all();
		dump_fn(NULL);
		dump_fn(NULL);
		dump_fn(NULL);
		dump_fn(NULL);	/* a fourth call must be a no-op */
		dump_layers("t");
		seq = s;
	}
	CHECK("G: dumps (all stages) are read-only", nwrites == 0);
	CHECK("G: the staged dump visited MMSYS, MUTEX0 and DSI0 exactly once", dump_stage == 3);

	printf("\n%d check(s) failed\n", fails);
	return fails ? 1 : 0;
}
