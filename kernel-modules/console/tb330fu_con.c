// SPDX-License-Identifier: GPL-2.0
/*
 * tb330fu_con - a text console on the framebuffer LK left running on the TB330FU: the kernel log scrolls by like on a normal Linux
 * boot, with the Tux logo at the top.
 *
 * The running kernel has no fbdev, VT or fbcon, so this is a small console driver of its own:
 *   - a struct console receives every printk line (CON_PRINTBUFFER replays what was logged before this module loaded);
 *   - the text goes into a character grid (cheap, callable from any context); a work item renders the grid with a bitmap font into a
 *     cached RAM image and copies that image into the write-combined framebuffer a few times per second;
 *   - the Tux logo (80x80, scaled) and a three line title sit above the scrolling text; lines that look like errors / warnings /
 *     "Started ..." / TB330FU messages get a colour; periodic chatter (watchdog, register dumps, ...) is kept off the screen;
 *   - if the kernel is dying (oops_in_progress) the picture is rendered synchronously, so a panic ends up on the screen;
 *   - when a DRM client takes the screen over (simpledrm writes MARK_REQ into the unused tail of scanline 0 on its first plane update
 *     and waits for MARK_ACK) the console stops, so that it never scribbles over the desktop.
 * The scan-out must already show the framebuffer region (tb330fu_ovl re-aims LK's overlay to it).
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/console.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/utsname.h>
#include <linux/jiffies.h>
#include <linux/sched/clock.h>

#include "fonts.h"
#include "tux80.h"

static unsigned long fb_base = 0x7c8e0000UL;
module_param(fb_base, ulong, 0444);
MODULE_PARM_DESC(fb_base, "physical address of the framebuffer (buffer 0 of LK's framebuffer region)");

static unsigned int width = 1200;
module_param(width, uint, 0444);
static unsigned int height = 1920;
module_param(height, uint, 0444);
static unsigned int stride = 4864;
module_param(stride, uint, 0444);
MODULE_PARM_DESC(stride, "bytes per scanline of the framebuffer");

static unsigned int rotate = 1;	/* default: landscape with the camera on top (the stock LK logo bitmap is stored the same way: reads top to bottom) */
module_param(rotate, uint, 0444);
MODULE_PARM_DESC(rotate, "console image rotation, quarter turns clockwise (0 = portrait, 1 = landscape with the camera on top [default], 3 = landscape the other way up, 2 = portrait upside down)");

static unsigned int fps = 8;
module_param(fps, uint, 0444);
MODULE_PARM_DESC(fps, "screen updates per second while the text changes");

static unsigned int tux_scale = 2;
module_param(tux_scale, uint, 0444);
MODULE_PARM_DESC(tux_scale, "integer scale of the 80x80 Tux logo");

static unsigned int font_sel = 1;
module_param_named(font, font_sel, uint, 0444);
MODULE_PARM_DESC(font, "0 = Terminus 16x32, 1 = Sun 12x22, 2 = 10x18");

static unsigned int quiet = 1;
module_param(quiet, uint, 0444);
MODULE_PARM_DESC(quiet, "1 = keep watchdog / register dump / device node chatter off the screen (it stays in the kernel log), 0 = show everything");

#define MARK_REQ	0x54583330u	/* "TX30": a DRM client wants the screen (written by simpledrm) */
#define MARK_ACK	0x54583331u	/* "TX31": the console has stopped (written here) */
#define MARK_REQ_OFF	(4 * 1210)	/* scanline 0, pixel 1210: beyond the 1200 visible pixels of the 1216 pixel scanline */
#define MARK_ACK_OFF	(4 * 1211)

struct fnt {
	const unsigned char *data;
	unsigned int w, h;
};
static const struct fnt fonts[] = {
	{ font_ter16x32, 16, 32 },
	{ font_sun12x22, 12, 22 },
	{ font_10x18, 10, 18 },
};

enum { C_ERR, C_WARN, C_TB, C_OK, C_DEF, C_N };	/* lower value = higher priority */
static const u32 palette[C_N] = {
	0xffff6464,	/* errors */
	0xffffd060,	/* warnings */
	0xff60e0ff,	/* TB330FU */
	0xff70ff70,	/* Started / Finished / Reached target */
	0xffc8c8c8,	/* everything else */
};

static const struct fnt *fnt;
static unsigned int gw, gh;		/* glyph cell */
static unsigned int cw, ch;		/* console image size (before rotation) */
static unsigned int cols, rows, hdr_rows, trows;
static char *cells, *snap_cells;	/* trows * cols characters (live / snapshot) */
static u8 *rowcol, *snap_rowcol;	/* colour class per text row */
static unsigned int cur_row, cur_col, snap_row, snap_col;
static u32 nlines, hidden, snap_lines, snap_hidden;
static bool dirty, stopped, registered;
static u32 *shadow, *rot_img, *tux_img;
static void __iomem *fb;
static DEFINE_RAW_SPINLOCK(lock);
static DEFINE_RAW_SPINLOCK(render_lock);
static struct delayed_work render_work;
static unsigned int last_blink;
static char release[65];		/* utsname()->release, copied at init: current->nsproxy may be gone in a dying task */

/* In a dying kernel the lock may belong to a CPU that will never run again: take it if possible, carry on without it otherwise. */
static bool grid_lock(unsigned long *flags)
{
	if (oops_in_progress)
		return raw_spin_trylock_irqsave(&lock, *flags);
	raw_spin_lock_irqsave(&lock, *flags);
	return true;
}

/* ------------------------------------------------------------------------------------------------------------- text grid */
/* messages that repeat all the time and would drown the boot log on the screen (the kernel log itself keeps them) */
static const char *const noise[] = {
	"[wdk-", "[wdtk]", "CNTCV_CTL", "UTC;android", "tb330fu_ovl:", "tb330fu-devd:", "tb330fu_blklog: snapshot", "audit: type=1334",
};

static bool has(const char *s, unsigned int n, const char *pat)
{
	unsigned int pl = strlen(pat), i;

	for (i = 0; i + pl <= n; i++)
		if (s[i] == pat[0] && !memcmp(s + i, pat, pl))
			return true;
	return false;
}

static bool is_noise(const char *s, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(noise); i++)
		if (has(s, n, noise[i]))
			return true;
	return false;
}

static u8 classify(const char *s)
{
	static const char *const err[] = { "Failed", "failed", "FAILED", "error", "Error", "ERROR", "Oops", "BUG:", "panic", "Call trace",
					   "segfault", "denied" };
	static const char *const warn[] = { "WARNING", "warning", "Warning", "timed out", "Timed out", "deprecated", "unsupported" };
	static const char *const ok[] = { "Started", "Finished", "Reached target", "Listening" };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(err); i++)
		if (strstr(s, err[i]))
			return C_ERR;
	for (i = 0; i < ARRAY_SIZE(warn); i++)
		if (strstr(s, warn[i]))
			return C_WARN;
	if (strstr(s, "TB330FU") || strstr(s, "tb330fu"))
		return C_TB;
	for (i = 0; i < ARRAY_SIZE(ok); i++)
		if (strstr(s, ok[i]))
			return C_OK;
	return C_DEF;
}

static void finish_row(unsigned int r)
{
	char tmp[200];
	unsigned int n = min_t(unsigned int, cols, sizeof(tmp) - 1);
	u8 c;

	memcpy(tmp, cells + r * cols, n);
	tmp[n] = 0;
	c = classify(tmp);
	if (c < rowcol[r])	/* only ever raise the priority: a wrapped line inherits the colour of its first row */
		rowcol[r] = c;
}

static void scroll_up(void)
{
	memmove(cells, cells + cols, (trows - 1) * cols);
	memset(cells + (trows - 1) * cols, ' ', cols);
	memmove(rowcol, rowcol + 1, trows - 1);
	rowcol[trows - 1] = C_DEF;
}

static void next_row(bool wrapped)
{
	u8 inherit;

	finish_row(cur_row);
	inherit = wrapped ? rowcol[cur_row] : C_DEF;
	cur_col = 0;
	if (++cur_row >= trows) {
		scroll_up();
		cur_row = trows - 1;
	}
	rowcol[cur_row] = inherit;
}

static void put_char(unsigned char c)
{
	if (c == '\n') {
		next_row(false);
		nlines++;
		return;
	}
	if (c == '\r') {
		cur_col = 0;
		return;
	}
	if (c == '\t') {
		unsigned int n = 8 - (cur_col & 7);

		while (n--)
			put_char(' ');
		return;
	}
	if (c < 0x20 || c == 0x7f)
		return;
	if (c >= 0x80)
		c = '?';
	if (cur_col >= cols)
		next_row(true);
	cells[cur_row * cols + cur_col++] = c;
}

/* ----------------------------------------------------------------------------------------------------------- rendering */
/* one glyph at (px, py), every font pixel becomes sc x sc screen pixels */
static void draw_glyph(unsigned int px, unsigned int py, unsigned char chr, u32 fg, u32 bg, unsigned int sc)
{
	const unsigned char *g = fnt->data + (unsigned int)chr * 2 * gh;
	unsigned int r, c, i;

	for (r = 0; r < gh; r++) {
		unsigned int bits = ((unsigned int)g[2 * r] << 8) | g[2 * r + 1];
		u32 *p = shadow + (size_t)(py + r * sc) * cw + px;

		if (sc == 1) {
			for (c = 0; c < gw; c++)
				p[c] = (bits & (0x8000u >> c)) ? fg : bg;
			continue;
		}
		for (c = 0; c < gw; c++) {
			u32 v = (bits & (0x8000u >> c)) ? fg : bg;

			for (i = 0; i < sc; i++)
				p[c * sc + i] = v;
		}
		for (i = 1; i < sc; i++)
			memcpy(p + (size_t)i * cw, p, (size_t)gw * sc * 4);
	}
}

static void draw_text(unsigned int px, unsigned int py, const char *s, u32 fg, unsigned int sc)
{
	while (*s && px + gw * sc <= cw && py + gh * sc <= ch) {
		draw_glyph(px, py, (unsigned char)*s++, fg, 0xff000000, sc);
		px += gw * sc;
	}
}

static void fill(unsigned int x0, unsigned int y0, unsigned int w, unsigned int h, u32 c)
{
	unsigned int x, y;

	for (y = y0; y < y0 + h && y < ch; y++)
		for (x = x0; x < x0 + w && x < cw; x++)
			shadow[(size_t)y * cw + x] = c;
}

static void render_frame(void)
{
	unsigned long flags;
	unsigned int r, c, blink = (jiffies / (HZ / 2)) & 1, tx, y2;
	bool locked;
	char buf[96];
	u64 up;

	/* snapshot the grid so that printk on other CPUs is never held up by the (slow) drawing */
	locked = grid_lock(&flags);
	memcpy(snap_cells, cells, trows * cols);
	memcpy(snap_rowcol, rowcol, trows);
	snap_row = cur_row;
	snap_col = cur_col;
	snap_lines = nlines;
	snap_hidden = hidden;
	dirty = false;
	if (locked)
		raw_spin_unlock_irqrestore(&lock, flags);

	/* header: black, Tux, three lines of title, a rule underneath */
	fill(0, 0, cw, hdr_rows * gh, 0xff000000);
	{
		unsigned int tw = TUX_W * tux_scale, th = TUX_H * tux_scale, y;

		for (y = 0; y < th && 16 + y < ch; y++)
			memcpy(shadow + (size_t)(16 + y) * cw + 16, tux_img + (size_t)y * tw, min_t(size_t, tw, cw - 16) * 4);
		tx = 16 + tw + 24;
		draw_text(tx, 16, "Linux on TB330FU", 0xffffffff, 2);
		y2 = 16 + 2 * gh + 8;
		draw_text(tx, y2, release, 0xff60e0ff, 1);
		up = div_u64(sched_clock(), 1000000);
		snprintf(buf, sizeof(buf), "MT8786 | up %llu.%03llu s | %u lines (+%u quiet)", (unsigned long long)(up / 1000),
			 (unsigned long long)(up % 1000), snap_lines, snap_hidden);
		draw_text(tx, y2 + gh + 6, buf, 0xffc8c8c8, 1);
		fill(0, hdr_rows * gh - 4, cw, 2, 0xff404040);
	}
	/* scrolling text */
	for (r = 0; r < trows; r++) {
		u32 fg = palette[snap_rowcol[r] < C_N ? snap_rowcol[r] : C_DEF];
		unsigned int ty = (hdr_rows + r) * gh;

		for (c = 0; c < cols; c++)
			draw_glyph(c * gw, ty, (unsigned char)snap_cells[r * cols + c], fg, 0xff000000, 1);
	}
	/* cursor: an underline that blinks */
	if (blink)
		fill(snap_col * gw, (hdr_rows + snap_row) * gh + gh - gh / 8 - 1, gw, gh / 8 + 1, 0xffffffff);
	last_blink = blink;
}

static void blit(void)
{
	unsigned int x, y;

	if (!rotate) {
		for (y = 0; y < height; y++)
			memcpy_toio(fb + (size_t)y * stride, shadow + (size_t)y * cw, width * 4);
		return;
	}
	for (y = 0; y < height; y++) {
		u32 *d = rot_img + (size_t)y * width;

		switch (rotate & 3) {
		case 1:	/* 90 degrees clockwise: fb(x, y) = console(y, ch - 1 - x) */
			for (x = 0; x < width; x++)
				d[x] = shadow[(size_t)(ch - 1 - x) * cw + y];
			break;
		case 2:	/* 180 degrees */
			for (x = 0; x < width; x++)
				d[x] = shadow[(size_t)(ch - 1 - y) * cw + (cw - 1 - x)];
			break;
		default:	/* 270 degrees clockwise: fb(x, y) = console(cw - 1 - y, x) */
			for (x = 0; x < width; x++)
				d[x] = shadow[(size_t)x * cw + (cw - 1 - y)];
			break;
		}
		memcpy_toio(fb + (size_t)y * stride, d, width * 4);
	}
}

static void render_now(void)
{
	unsigned long flags;
	bool locked = raw_spin_trylock_irqsave(&render_lock, flags);

	if (!locked && !oops_in_progress)	/* somebody else is drawing right now */
		return;
	render_frame();
	blit();
	if (locked)
		raw_spin_unlock_irqrestore(&render_lock, flags);
}

/* -------------------------------------------------------------------------------------------------------------- console */
static void con_write(struct console *c, const char *s, unsigned int n)
{
	unsigned long flags;
	unsigned int i;
	bool locked;

	if (stopped || !cells)
		return;
	if (quiet && is_noise(s, n)) {
		hidden++;
		return;
	}
	locked = grid_lock(&flags);
	for (i = 0; i < n; i++)
		put_char((unsigned char)s[i]);
	dirty = true;
	if (locked)
		raw_spin_unlock_irqrestore(&lock, flags);
	if (oops_in_progress)
		render_now();
}

static struct console con = {
	.name = "tb330fb",
	.write = con_write,
	.flags = CON_PRINTBUFFER | CON_ENABLED,
	.index = -1,
};

static void free_buffers(void)
{
	vfree(shadow);
	vfree(rot_img);
	vfree(tux_img);
	vfree(cells);
	vfree(snap_cells);
	vfree(rowcol);
	vfree(snap_rowcol);
	shadow = rot_img = tux_img = NULL;
	cells = snap_cells = NULL;
	rowcol = snap_rowcol = NULL;
}

static void render_fn(struct work_struct *w)
{
	unsigned long flags;

	if (stopped)
		return;
	if (readl_relaxed(fb + MARK_REQ_OFF) == MARK_REQ) {
		/* a DRM client wants the screen: stop for good, then acknowledge */
		raw_spin_lock_irqsave(&lock, flags);
		stopped = true;
		raw_spin_unlock_irqrestore(&lock, flags);
		if (registered) {
			unregister_console(&con);
			registered = false;
		}
		pr_info("tb330fu_con: a DRM client took the screen over, console stopped\n");
		wmb();
		writel_relaxed(MARK_ACK, fb + MARK_ACK_OFF);
		free_buffers();
		return;
	}
	if (dirty || (((jiffies / (HZ / 2)) & 1) != last_blink))
		render_now();
	queue_delayed_work(system_long_wq, &render_work, msecs_to_jiffies(1000 / (fps ? fps : 8)));
}

static int __init con_init(void)
{
	unsigned int tw, th, x, y;
	size_t shadow_bytes;

	if (rotate > 3 || !width || !height || stride < width * 4 || tux_scale < 1 || tux_scale > 4 || font_sel >= ARRAY_SIZE(fonts)) {
		pr_err("tb330fu_con: bad parameters (rotate=%u %ux%u stride=%u tux_scale=%u font=%u)\n", rotate, width, height, stride, tux_scale,
		       font_sel);
		return -EINVAL;
	}
	fnt = &fonts[font_sel];
	gw = fnt->w;
	gh = fnt->h;
	cw = (rotate & 1) ? height : width;
	ch = (rotate & 1) ? width : height;
	cols = cw / gw;
	rows = ch / gh;
	hdr_rows = DIV_ROUND_UP(16 + TUX_H * tux_scale + 16, gh);
	if (cols < 40 || rows <= hdr_rows + 4 || cols > 199 || 16 + 2 * gh + 8 + 2 * gh + 6 > 16 + TUX_H * tux_scale + 16) {
		pr_err("tb330fu_con: console %ux%u px gives %ux%u cells, not usable\n", cw, ch, cols, rows);
		return -EINVAL;
	}
	trows = rows - hdr_rows;
	shadow_bytes = (size_t)cw * ch * 4;
	shadow = vzalloc(shadow_bytes);
	rot_img = rotate ? vzalloc((size_t)width * height * 4) : NULL;
	tux_img = vzalloc((size_t)TUX_W * tux_scale * TUX_H * tux_scale * 4);
	cells = vmalloc(trows * cols);
	snap_cells = vmalloc(trows * cols);
	rowcol = vzalloc(trows);
	snap_rowcol = vzalloc(trows);
	if (!shadow || (rotate && !rot_img) || !tux_img || !cells || !snap_cells || !rowcol || !snap_rowcol) {
		pr_err("tb330fu_con: out of memory\n");
		free_buffers();
		return -ENOMEM;
	}
	strscpy(release, utsname()->release, sizeof(release));
	fb = ioremap_wc(fb_base, (size_t)stride * height);
	if (!fb) {
		pr_err("tb330fu_con: ioremap_wc(%#lx) failed\n", fb_base);
		free_buffers();
		return -ENOMEM;
	}
	memset(cells, ' ', trows * cols);
	memset(rowcol, C_DEF, trows);
	tw = TUX_W * tux_scale;
	th = TUX_H * tux_scale;
	for (y = 0; y < th; y++)
		for (x = 0; x < tw; x++) {
			const unsigned char *p = tux_pal[tux_idx[(y / tux_scale) * TUX_W + (x / tux_scale)]];

			tux_img[(size_t)y * tw + x] = 0xff000000u | ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
		}
	fill(0, 0, cw, ch, 0xff000000);
	writel_relaxed(0, fb + MARK_REQ_OFF);
	writel_relaxed(0, fb + MARK_ACK_OFF);
	render_now();
	pr_info("tb330fu_con: %ux%u cells of %ux%u px (%u text rows under a %u row header), console image %ux%u rotate=%u, %u fps, quiet=%u\n",
		cols, rows, gw, gh, trows, hdr_rows, cw, ch, rotate, fps ? fps : 8, quiet);
	INIT_DELAYED_WORK(&render_work, render_fn);
	register_console(&con);
	registered = true;
	queue_delayed_work(system_long_wq, &render_work, msecs_to_jiffies(100));
	return 0;
}

static void __exit con_exit(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&lock, flags);
	stopped = true;
	raw_spin_unlock_irqrestore(&lock, flags);
	cancel_delayed_work_sync(&render_work);
	if (registered)
		unregister_console(&con);
	if (fb)
		iounmap(fb);
	free_buffers();
}

module_init(con_init);
module_exit(con_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TB330FU: kernel-log text console with the Tux logo on LK's framebuffer");
