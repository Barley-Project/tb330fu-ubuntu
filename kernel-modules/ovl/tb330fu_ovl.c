// SPDX-License-Identifier: GPL-2.0
/*
 * tb330fu_ovl - read out, and (in visible, reversible steps) re-aim the display scan-out that the bootloader (LK) left running on the
 * TB330FU (MT6768, MediaTek DISP OVL0 / OVL0_2L).
 *
 * What the CN LK binary does (platform/mt6768/ddp_ovl.c, disassembled; register offsets from the same code):
 *   L_CON(n)   = base+0x30+0x20n : (fmt << 12) | alpha-enable (bit 8) | alpha | ... | 0x600     <- bits 9 and 10 = vertical + horizontal flip,
 *                                                                                                   set for EVERY RGB layer
 *   SRC_SIZE   = base+0x38+0x20n : (height << 16) | width
 *   OFFSET     = base+0x3c+0x20n
 *   PITCH      = base+0x44+0x20n : pitch in bytes
 *   RDMA_CTRL  = base+0xc0+0x20n : 1
 *   ADDR       = base+0xf40+0x20n: addr + pitch * (off_y + h - 1) + bpp * (off_x + w) + (bpp - 1)     <- the END of the layer, because the
 *                                                                                                          overlay reads it flipped
 *   ROI_SIZE 0x20 = (h << 16) | w, ROI_BGCLR 0x28, SRC_CON 0x2c (bit n = layer n enabled); module n sits at base + n * 0x1000.
 * LK drives OVL0 layer0 = framebuffer buffer 0/2 (pitch 4864) and layer1 = buffer 1 (pitch 4800).  The first version of this module wrote the
 * plain buffer START address into a flipped layer, so the overlay read memory BELOW the framebuffer and the panel went white; this version
 * always uses LK's own formula.
 *
 * What it does:
 *   1. dumps the raw registers of the display path (OVL0, OVL0_2L, MMSYS, MUTEX0, DSI0) and one decoded line per OVL layer (RDMA0 is left out
 *      on purpose: LK stopped using it, its clock may be gated, and reading a gated MM block can hang the bus);
 *   2. seq=1: start_ms after load it runs a short visible experiment on the layers that read the framebuffer region, restoring the ORIGINAL
 *      registers between the steps, so that a wrong guess can never leave the screen in a new state for long:
 *         P1  keep one layer (prefers pitch == stride), aim it at fb_base with LK's formula (flips untouched), disable the other fb layers
 *         P4  like P1, but with the flip bits cleared and the plain start address (the "unflipped" convention)
 *         final: P1 (final=1), P4 (final=2) or the original state (final=0), kept until shutdown
 *   3. final_ms after load it prints the decoded layers once more (this lands near the end of the persistent log).
 * Nothing is written when seq=0; a layer is only touched if it is enabled and its address lies inside the framebuffer region.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/io.h>
#include <linux/workqueue.h>
#include <linux/kernel.h>

static unsigned long fb_base = 0x7c8e0000UL;
module_param(fb_base, ulong, 0444);
MODULE_PARM_DESC(fb_base, "physical address of LK's framebuffer region (also the buffer the kept layer is aimed at)");

static unsigned long fb_size = 0x1f20000UL;
module_param(fb_size, ulong, 0444);
MODULE_PARM_DESC(fb_size, "size of the framebuffer region; layers whose address lies inside it are candidates");

static unsigned int width = 1200;
module_param(width, uint, 0444);
static unsigned int height = 1920;
module_param(height, uint, 0444);
static unsigned int stride = 4864;
module_param(stride, uint, 0444);
MODULE_PARM_DESC(stride, "bytes per scanline the kept layer is programmed with");

static int dump = 1;
module_param(dump, int, 0444);
MODULE_PARM_DESC(dump, "1 = dump the display registers at load");

static int seq = 2;
module_param(seq, int, 0444);
MODULE_PARM_DESC(seq, "2 = apply P1 (LK's own address formula) at load and keep it (default), 1 = run the P1 / P4 experiment sequence, 0 = nothing");

static int final = 1;
module_param(final, int, 0444);
MODULE_PARM_DESC(final, "state kept after the sequence: 0 = original, 1 = P1, 2 = P4");

static unsigned int start_ms = 12000;
module_param(start_ms, uint, 0444);
static unsigned int hold_ms = 8000;
module_param(hold_ms, uint, 0444);
static unsigned int gap_ms = 2000;
module_param(gap_ms, uint, 0444);
MODULE_PARM_DESC(start_ms, "milliseconds after load until the first experiment step");
MODULE_PARM_DESC(hold_ms, "how long an experiment variant stays on screen");
MODULE_PARM_DESC(gap_ms, "how long the original state is shown between two variants");

static unsigned int final_ms = 120000;
module_param(final_ms, uint, 0444);
MODULE_PARM_DESC(final_ms, "milliseconds after load for a last decoded dump of the layers (0 = none)");

#define OVL_EN			0x00c
#define OVL_ROI_SIZE		0x020
#define OVL_DATAPATH_CON	0x024
#define OVL_SRC_CON		0x02c
#define OVL_CON(n)		(0x030 + 0x20 * (n))
#define OVL_SRC_SIZE(n)		(0x038 + 0x20 * (n))
#define OVL_OFFSET(n)		(0x03c + 0x20 * (n))
#define OVL_PITCH(n)		(0x044 + 0x20 * (n))
#define OVL_RDMA_CTRL(n)	(0x0c0 + 0x20 * (n))
#define OVL_ADDR(n)		(0xf40 + 0x20 * (n))
#define CON_FLIPS		0x600u

struct blk {
	const char *name;
	unsigned long pa;
	unsigned int nlayers;	/* only for the OVLs */
	void __iomem *va;
};

static struct blk blks[] = {
	{ "MMSYS", 0x14000000UL, 0, NULL },
	{ "MUTEX0", 0x14001000UL, 0, NULL },
	{ "OVL0", 0x1400b000UL, 4, NULL },
	{ "OVL0_2L", 0x1400c000UL, 2, NULL },
	{ "DSI0", 0x14014000UL, 0, NULL },
};
#define NBLK ARRAY_SIZE(blks)
#define IDX_OVL0	2
#define IDX_OVL0_2L	3

struct lay {
	unsigned int ovl, n;
	u32 con, size, offs, pitch, addr;
};

static struct delayed_work seq_work;
static struct delayed_work final_work;
static struct lay cand[8];
static unsigned int nc, keep, step;
static u32 saved_src_con[2];

/* LK's ADDR register value for a layer of the given base / pitch / size (off = 0, 4 bytes per pixel) */
static u32 lk_addr(unsigned long base)
{
	return (u32)(base + (unsigned long)stride * (height - 1) + 4ul * width + 3);
}

static void dump_range(struct blk *b, unsigned int from, unsigned int to)
{
	unsigned int off, i;

	for (off = from; off < to; off += 32) {
		u32 w[8];

		for (i = 0; i < 8; i++)
			w[i] = readl_relaxed(b->va + off + 4 * i);
		pr_info("tb330fu_ovl: %s +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", b->name, off, w[0], w[1], w[2], w[3], w[4],
			w[5], w[6], w[7]);
	}
}

static void dump_layers(const char *tag)
{
	unsigned int o, n;

	for (o = IDX_OVL0; o <= IDX_OVL0_2L; o++) {
		struct blk *b = &blks[o];
		u32 en = readl_relaxed(b->va + OVL_EN);
		u32 src_con = readl_relaxed(b->va + OVL_SRC_CON);

		pr_info("tb330fu_ovl: [%s] %s en=%08x src_con=%08x roi=%08x bgclr=%08x datapath=%08x\n", tag, b->name, en, src_con,
			readl_relaxed(b->va + OVL_ROI_SIZE), readl_relaxed(b->va + 0x028), readl_relaxed(b->va + OVL_DATAPATH_CON));
		for (n = 0; n < b->nlayers; n++) {
			u32 con = readl_relaxed(b->va + OVL_CON(n));
			u32 pitch = readl_relaxed(b->va + OVL_PITCH(n));
			u32 addr = readl_relaxed(b->va + OVL_ADDR(n));
			u32 base = (con & CON_FLIPS) == CON_FLIPS ?
				   addr - ((pitch & 0xffff) * (height - 1) + 4 * width + 3) : addr;

			pr_info("tb330fu_ovl: [%s] %s L%u on=%u con=%08x size=%08x offset=%08x pitch=%08x addr=%08x rdma=%08x flips=%u base=%08x\n",
				tag, b->name, n, !!(src_con & (1u << n)), con, readl_relaxed(b->va + OVL_SRC_SIZE(n)),
				readl_relaxed(b->va + OVL_OFFSET(n)), pitch, addr, readl_relaxed(b->va + OVL_RDMA_CTRL(n)),
				!!((con & CON_FLIPS) == CON_FLIPS), base);
		}
	}
}

/* One block per call, so that the dump can be spread over time and a bus hang in a late block leaves the early blocks in the persistent log. */
static void dump_block(unsigned int i)
{
	struct blk *b = &blks[i];

	pr_info("tb330fu_ovl: ---- %s @ %#lx\n", b->name, b->pa);
	switch (i) {
	case 0:	/* MMSYS: clock gates, resets, path selection */
		dump_range(b, 0x000, 0x200);
		dump_range(b, 0xf00, 0x1000);
		break;
	case 1:	/* MUTEX0 */
		dump_range(b, 0x000, 0x100);
		break;
	case IDX_OVL0:
	case IDX_OVL0_2L:
		dump_range(b, 0x000, 0x100);
		dump_range(b, 0xf40, 0xfc0);	/* the four layer address registers only */
		break;
	default:	/* DSI0: control / size / timing registers only */
		dump_range(b, 0x000, 0x100);
		break;
	}
}

static unsigned int dump_stage;
static struct delayed_work dump_work;

/* stage 0 (in init): the OVLs, which matter most; stages 1..3 every 2 s: MMSYS, MUTEX0, DSI0 */
static void dump_fn(struct work_struct *w)
{
	static const unsigned int order[] = { 0, 1, 4 };

	if (dump_stage >= ARRAY_SIZE(order))
		return;
	dump_block(order[dump_stage++]);
	if (dump_stage < ARRAY_SIZE(order))
		schedule_delayed_work(&dump_work, msecs_to_jiffies(2000));
}

static void dump_all(void)
{
	dump_block(IDX_OVL0);
	dump_block(IDX_OVL0_2L);
	dump_layers("dump");
	INIT_DELAYED_WORK(&dump_work, dump_fn);
	schedule_delayed_work(&dump_work, msecs_to_jiffies(2000));
}

/* find the enabled layers that read the framebuffer region and remember their original registers */
static void collect(void)
{
	unsigned int o, n, i;
	bool have_keep = false;

	nc = 0;
	keep = 0;
	for (o = IDX_OVL0; o <= IDX_OVL0_2L; o++) {
		struct blk *b = &blks[o];
		u32 src_con = readl_relaxed(b->va + OVL_SRC_CON);

		saved_src_con[o - IDX_OVL0] = src_con;
		for (n = 0; n < b->nlayers; n++) {
			u32 addr = readl_relaxed(b->va + OVL_ADDR(n));

			if (!(src_con & (1u << n)) || addr < fb_base || addr >= fb_base + fb_size)
				continue;
			if (nc >= ARRAY_SIZE(cand))
				continue;
			cand[nc].ovl = o;
			cand[nc].n = n;
			cand[nc].con = readl_relaxed(b->va + OVL_CON(n));
			cand[nc].size = readl_relaxed(b->va + OVL_SRC_SIZE(n));
			cand[nc].offs = readl_relaxed(b->va + OVL_OFFSET(n));
			cand[nc].pitch = readl_relaxed(b->va + OVL_PITCH(n));
			cand[nc].addr = addr;
			nc++;
		}
	}
	for (i = 0; i < nc; i++) {
		pr_info("tb330fu_ovl: candidate %s L%u con=%08x addr=%08x pitch=%08x size=%08x offset=%08x\n", blks[cand[i].ovl].name,
			cand[i].n, cand[i].con, cand[i].addr, cand[i].pitch, cand[i].size, cand[i].offs);
		if (!have_keep && (cand[i].pitch & 0xffff) == stride) {
			keep = i;
			have_keep = true;
		}
	}
	if (nc)
		pr_info("tb330fu_ovl: keeper = %s L%u (%s)\n", blks[cand[keep].ovl].name, cand[keep].n,
			have_keep ? "pitch matches stride" : "no layer has the wanted pitch, first candidate");
}

static void restore(void)
{
	unsigned int i;

	for (i = 0; i < nc; i++) {
		struct blk *b = &blks[cand[i].ovl];
		unsigned int n = cand[i].n;

		writel(cand[i].con, b->va + OVL_CON(n));
		writel(cand[i].size, b->va + OVL_SRC_SIZE(n));
		writel(cand[i].offs, b->va + OVL_OFFSET(n));
		writel(cand[i].pitch, b->va + OVL_PITCH(n));
		writel(cand[i].addr, b->va + OVL_ADDR(n));
	}
	writel(saved_src_con[0], blks[IDX_OVL0].va + OVL_SRC_CON);
	writel(saved_src_con[1], blks[IDX_OVL0_2L].va + OVL_SRC_CON);
	wmb();
}

static void apply(int variant)
{
	struct blk *kb = &blks[cand[keep].ovl];
	unsigned int kn = cand[keep].n, i;
	u32 pitch = cand[keep].pitch;

	for (i = 0; i < nc; i++) {
		struct blk *b = &blks[cand[i].ovl];

		if (i == keep)
			continue;
		writel(readl(b->va + OVL_SRC_CON) & ~(1u << cand[i].n), b->va + OVL_SRC_CON);
	}
	writel((pitch & 0xffff0000u) | stride, kb->va + OVL_PITCH(kn));
	writel((height << 16) | width, kb->va + OVL_SRC_SIZE(kn));
	if (variant == 4) {
		writel(cand[keep].con & ~CON_FLIPS, kb->va + OVL_CON(kn));
		writel((u32)fb_base, kb->va + OVL_ADDR(kn));
	} else {
		writel(cand[keep].con, kb->va + OVL_CON(kn));
		writel(lk_addr(fb_base), kb->va + OVL_ADDR(kn));
	}
	wmb();
	pr_info("tb330fu_ovl: applied P%d on %s L%u: addr=%08x con=%08x pitch=%08x size=%08x, %u other layer(s) disabled\n", variant, kb->name,
		kn, readl(kb->va + OVL_ADDR(kn)), readl(kb->va + OVL_CON(kn)), readl(kb->va + OVL_PITCH(kn)), readl(kb->va + OVL_SRC_SIZE(kn)), nc - 1);
}

static void seq_fn(struct work_struct *w)
{
	unsigned int next_ms = 0;

	switch (step) {
	case 0:
		pr_info("tb330fu_ovl: sequence step 0: collecting the layers that read %#lx..%#lx\n", fb_base, fb_base + fb_size);
		collect();
		if (!nc) {
			pr_info("tb330fu_ovl: no enabled layer reads the framebuffer region, nothing written\n");
			return;
		}
		apply(1);
		next_ms = hold_ms;
		break;
	case 1:
		restore();
		pr_info("tb330fu_ovl: sequence step 1: original registers restored\n");
		next_ms = gap_ms;
		break;
	case 2:
		apply(4);
		next_ms = hold_ms;
		break;
	case 3:
		restore();
		pr_info("tb330fu_ovl: sequence step 3: original registers restored\n");
		next_ms = gap_ms;
		break;
	default:
		if (final == 1)
			apply(1);
		else if (final == 2)
			apply(4);
		else
			pr_info("tb330fu_ovl: sequence done, original state kept\n");
		dump_layers("final-state");
		return;
	}
	step++;
	schedule_delayed_work(&seq_work, msecs_to_jiffies(next_ms));
}

static void final_fn(struct work_struct *w)
{
	dump_layers("late");
}

static int __init ovl_init(void)
{
	unsigned int i;

	pr_info("tb330fu_ovl: fb_base=%#lx fb_size=%#lx %ux%u stride=%u dump=%d seq=%d final=%d start=%ums hold=%ums gap=%ums\n", fb_base, fb_size,
		width, height, stride, dump, seq, final, start_ms, hold_ms, gap_ms);
	for (i = 0; i < NBLK; i++) {
		blks[i].va = ioremap(blks[i].pa, 0x1000);
		if (!blks[i].va) {
			pr_err("tb330fu_ovl: ioremap(%s @ %#lx) failed\n", blks[i].name, blks[i].pa);
			while (i--)
				iounmap(blks[i].va);
			return -ENOMEM;
		}
	}
	if (dump)
		dump_all();
	if (seq == 2) {
		/* aim the scan-out at the framebuffer right away and keep it (the boot console draws there from now on) */
		collect();
		if (nc)
			apply(1);
		else
			pr_info("tb330fu_ovl: no enabled layer reads the framebuffer region, nothing written\n");
	} else if (seq) {
		INIT_DELAYED_WORK(&seq_work, seq_fn);
		schedule_delayed_work(&seq_work, msecs_to_jiffies(start_ms));
	}
	if (final_ms) {
		INIT_DELAYED_WORK(&final_work, final_fn);
		schedule_delayed_work(&final_work, msecs_to_jiffies(final_ms));
	}
	return 0;
}

static void __exit ovl_exit(void)
{
	unsigned int i;

	if (dump)
		cancel_delayed_work_sync(&dump_work);
	if (seq == 1)
		cancel_delayed_work_sync(&seq_work);
	if (final_ms)
		cancel_delayed_work_sync(&final_work);
	for (i = 0; i < NBLK; i++)
		iounmap(blks[i].va);
}

module_init(ovl_init);
module_exit(ovl_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TB330FU: dump the display path registers and re-aim LK's OVL scan-out in visible, reversible steps");
