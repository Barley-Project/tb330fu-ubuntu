// SPDX-License-Identifier: GPL-2.0
/*
 * tb330fu_fb - give the Linux DRM stack the framebuffer the bootloader (LK) already programmed into the display pipeline.
 *
 * Why: the TB330FU kernel has no display driver loaded yet, so the panel keeps showing LK's boot logo (the DISP/DSI pipeline
 * of the SoC keeps scanning out of LK's reserved "framebuffer" memory, mblock-17-framebuffer, 0x7c8e0000.., nomap).
 * This module
 *   1. optionally paints a test pattern (colour bars, gradient, checkerboard, corner markers, buffer-index markers) into that
 *      memory so that a human can read geometry / byte order / which buffer is scanned from the screen, plus a heartbeat square
 *      that blinks for hb_secs seconds so that a human can see that the kernel is alive;
 *   2. registers a "simple-framebuffer" platform device (simplefb_platform_data) so that the simpledrm DRM driver binds to the
 *      memory and exposes a normal KMS device (/dev/dri/card0) for KWin.
 *
 * Nothing here touches the display hardware itself; it only writes to the scan-out memory.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/platform_data/simplefb.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/ioport.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>

static unsigned long fb_base = 0x7c8e0000UL;
module_param(fb_base, ulong, 0444);
MODULE_PARM_DESC(fb_base, "physical address of LK's framebuffer memory");

static unsigned long fb_size = 0x1f20000UL;
module_param(fb_size, ulong, 0444);
MODULE_PARM_DESC(fb_size, "size of the reserved framebuffer memory (all of it is painted when pattern=1)");

static unsigned int width = 1200;
module_param(width, uint, 0444);
static unsigned int height = 1920;
module_param(height, uint, 0444);
static unsigned int stride = 4864;
module_param(stride, uint, 0444);
MODULE_PARM_DESC(stride, "bytes per scanline (LK programs OVL0 layer0 with pitch=4864, fmt=eBGRA8888, 1200x1920 at 0x7c8e0000)");

static char *format = "a8r8g8b8";
module_param(format, charp, 0444);
MODULE_PARM_DESC(format, "simplefb format name (x8r8g8b8, a8r8g8b8, x8b8g8r8, a8b8g8r8, r5g6b5, ...)");

static int pattern;	/* 0 - the boot console (tb330fu_con) draws on the screen; pattern=1 gives the old test pattern back */
module_param(pattern, int, 0444);
MODULE_PARM_DESC(pattern, "1 = paint the test pattern at load (assumes 32 bit pixels), 0 = leave the screen alone");

static int register_fb = 1;
module_param(register_fb, int, 0444);
MODULE_PARM_DESC(register_fb, "1 = register the simple-framebuffer platform device for simpledrm");

static unsigned int hb_secs;	/* 0 = no heartbeat blink */
module_param(hb_secs, uint, 0444);
MODULE_PARM_DESC(hb_secs, "seconds the heartbeat square keeps blinking (0 = no heartbeat)");

static void __iomem *map;
static struct platform_device *pdev;
static struct simplefb_platform_data pdata;
static struct delayed_work hb_work;
static unsigned long hb_t0;
static unsigned int hb_n;

/* ---- screenshot interface: /sys/kernel/tb330fu/fb0 = the scanned buffer (buffer 0: stride x height bytes, a8r8g8b8 as the panel sees it, i.e.
 * the native portrait orientation) read back raw; /sys/kernel/tb330fu/info = geometry.  Root only.  A PC converts it with 393_raw2png.py. */
static struct kobject *dbg_kobj;
static void __iomem *dbg_map;

static ssize_t fb0_read(struct file *filp, struct kobject *kobj, struct bin_attribute *attr, char *buf, loff_t off, size_t count)
{
	size_t total = (size_t)stride * height;

	if (!dbg_map || off < 0 || (size_t)off >= total)
		return 0;
	if (count > total - (size_t)off)
		count = total - (size_t)off;
	memcpy_fromio(buf, dbg_map + off, count);
	return count;
}

static struct bin_attribute fb0_attr = {
	.attr = { .name = "fb0", .mode = 0400 },
	.read = fb0_read,
};

static ssize_t info_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "fb_base=%#lx fb_size=%#lx width=%u height=%u stride=%u format=%s\n", fb_base, fb_size, width, height, stride, format);
}

static struct kobj_attribute info_attr = __ATTR_RO(info);

static void dbg_init(void)
{
	dbg_map = ioremap_wc(fb_base, (size_t)stride * height);
	if (!dbg_map)
		return;
	dbg_kobj = kobject_create_and_add("tb330fu", kernel_kobj);
	if (!dbg_kobj)
		goto err;
	fb0_attr.size = (size_t)stride * height;
	if (sysfs_create_bin_file(dbg_kobj, &fb0_attr) || sysfs_create_file(dbg_kobj, &info_attr.attr))
		goto err;
	pr_info("tb330fu_fb: screenshot interface /sys/kernel/tb330fu/fb0 (%u bytes)\n", stride * height);
	return;
err:
	pr_err("tb330fu_fb: could not create the screenshot interface\n");
	if (dbg_kobj) {
		kobject_put(dbg_kobj);
		dbg_kobj = NULL;
	}
	iounmap(dbg_map);
	dbg_map = NULL;
}

static void dbg_exit(void)
{
	if (dbg_kobj) {
		sysfs_remove_bin_file(dbg_kobj, &fb0_attr);
		sysfs_remove_file(dbg_kobj, &info_attr.attr);
		kobject_put(dbg_kobj);
		dbg_kobj = NULL;
	}
	if (dbg_map) {
		iounmap(dbg_map);
		dbg_map = NULL;
	}
}

#define COL(a, r, g, b) (((u32)(a) << 24) | ((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))

static inline void px(u32 __iomem *base, unsigned int stride_px, unsigned int x, unsigned int y, unsigned int w,
		      unsigned int h, u32 c)
{
	if (x < w && y < h)
		writel_relaxed(c, base + (size_t)y * stride_px + x);
}

static void rect(u32 __iomem *base, unsigned int stride_px, unsigned int w, unsigned int h, unsigned int x0, unsigned int y0,
		 unsigned int rw, unsigned int rh, u32 c)
{
	unsigned int x, y;

	for (y = y0; y < y0 + rh && y < h; y++)
		for (x = x0; x < x0 + rw && x < w; x++)
			writel_relaxed(c, base + (size_t)y * stride_px + x);
}

/* 5x7 glyphs, bit 4 = leftmost pixel: the buffer index digit and an asymmetric 'F' (reveals rotation / mirroring) */
static const u8 glyph_digit[3][7] = {
	{ 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e },	/* 0 */
	{ 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e },	/* 1 */
	{ 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f },	/* 2 */
};
static const u8 glyph_f[7] = { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 };

static void glyph(u32 __iomem *b, unsigned int stride_px, unsigned int w, unsigned int h, unsigned int x0, unsigned int y0,
		  unsigned int scale, const u8 *g, u32 c)
{
	unsigned int r, col;

	for (r = 0; r < 7; r++)
		for (col = 0; col < 5; col++)
			if (g[r] & (0x10 >> col))
				rect(b, stride_px, w, h, x0 + col * scale, y0 + r * scale, scale, scale, c);
}

static void paint_one(u32 __iomem *b, unsigned int stride_px, unsigned int w, unsigned int h, unsigned int idx)
{
	static const u32 bars[4] = { COL(255, 255, 0, 0), COL(255, 0, 255, 0), COL(255, 0, 0, 255), COL(255, 255, 255, 255) };
	unsigned int x, y, k;

	/* whole buffer: dark background with a coarse checkerboard of 120x120 squares */
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			writel_relaxed((((x / 120) + (y / 120)) & 1) ? COL(255, 192, 192, 192) : COL(255, 32, 32, 32),
				       b + (size_t)y * stride_px + x);
	/* rows 0..119: four colour bars red / green / blue / white (byte-order check) */
	for (k = 0; k < 4; k++)
		rect(b, stride_px, w, h, k * (w / 4), 0, w / 4, 120, bars[k]);
	/* rows 120..239: horizontal gray ramp with a white tick every 100 px (stride / shear check) */
	for (y = 120; y < 240; y++)
		for (x = 0; x < w; x++) {
			u32 g = (x * 255) / (w ? w : 1);

			px(b, stride_px, x, y, w, h, (x % 100 == 0) ? COL(255, 255, 255, 255) : COL(255, g, g, g));
		}
	/* corner markers: top-left magenta, top-right yellow, bottom-left cyan, bottom-right orange (orientation / crop) */
	rect(b, stride_px, w, h, 0, 240, 100, 100, COL(255, 255, 0, 255));
	rect(b, stride_px, w, h, w - 100, 240, 100, 100, COL(255, 255, 255, 0));
	rect(b, stride_px, w, h, 0, h - 100, 100, 100, COL(255, 0, 255, 255));
	rect(b, stride_px, w, h, w - 100, h - 100, 100, 100, COL(255, 255, 128, 0));
	/* buffer-index markers: idx+1 white squares at y = 400 (which buffer is being scanned out) */
	for (k = 0; k <= idx; k++)
		rect(b, stride_px, w, h, 140 + k * 70, 400, 50, 50, COL(255, 255, 255, 255));
	/* 2 px white frame around the visible area (cropping check) */
	for (x = 0; x < w; x++)
		for (k = 0; k < 2; k++) {
			px(b, stride_px, x, k, w, h, COL(255, 255, 255, 255));
			px(b, stride_px, x, h - 1 - k, w, h, COL(255, 255, 255, 255));
		}
	for (y = 0; y < h; y++)
		for (k = 0; k < 2; k++) {
			px(b, stride_px, k, y, w, h, COL(255, 255, 255, 255));
			px(b, stride_px, w - 1 - k, y, w, h, COL(255, 255, 255, 255));
		}
	/* big buffer digit (0 = fb_base, 1, 2) in a white box in the middle, and an asymmetric 'F' next to it (rotation / mirror check) */
	rect(b, stride_px, w, h, 200, 800, 320, 320, COL(255, 255, 255, 255));
	glyph(b, stride_px, w, h, 260, 840, 40, glyph_digit[idx < 3 ? idx : 0], COL(255, 0, 0, 0));
	rect(b, stride_px, w, h, 620, 830, 300, 260, COL(255, 255, 255, 160));
	glyph(b, stride_px, w, h, 710, 876, 24, glyph_f, COL(255, 0, 0, 0));
	/* the unused tail of every scanline (pitch minus width) in pink: a scan that starts mid-row shows it as a thin line at the wrap seam */
	for (y = 0; y < h; y++)
		for (x = w; x < stride_px; x++)
			writel_relaxed(COL(255, 255, 0, 128), b + (size_t)y * stride_px + x);
}

static void heartbeat(struct work_struct *w)
{
	unsigned int n, nb = fb_size / ((size_t)stride * height);
	u32 c = (hb_n++ & 1) ? COL(255, 255, 255, 255) : COL(255, 0, 0, 0);

	if (!map)
		return;
	for (n = 0; n < nb; n++) {
		u32 __iomem *b = (u32 __iomem *)(map + (size_t)n * stride * height);

		rect(b, stride / 4, width, height, width / 2 - 30, height - 200, 60, 60, c);
	}
	if (jiffies_to_msecs(jiffies - hb_t0) / 1000 < hb_secs)
		schedule_delayed_work(&hb_work, HZ);
}

static int __init fb_init(void)
{
	struct resource res = DEFINE_RES_MEM(fb_base, (size_t)stride * height);
	unsigned int n, nb;

	if (!stride || !height || !width || stride < width * 4 || (size_t)stride * height > fb_size) {
		pr_err("tb330fu_fb: geometry %ux%u stride %u does not fit into %#lx bytes\n", width, height, stride, fb_size);
		return -EINVAL;
	}
	pr_info("tb330fu_fb: fb_base=%#lx fb_size=%#lx %ux%u stride=%u format=%s pattern=%d register=%d hb=%us\n", fb_base, fb_size,
		width, height, stride, format, pattern, register_fb, hb_secs);

	if (pattern) {
		map = ioremap_wc(fb_base, fb_size);
		if (!map) {
			pr_err("tb330fu_fb: ioremap_wc(%#lx, %#lx) failed\n", fb_base, fb_size);
			return -ENOMEM;
		}
		nb = fb_size / ((size_t)stride * height);
		for (n = 0; n < nb; n++)
			paint_one((u32 __iomem *)(map + (size_t)n * stride * height), stride / 4, width, height, n);
		wmb();
		pr_info("tb330fu_fb: test pattern painted into %u buffer(s) of %u bytes\n", nb, stride * height);
		if (hb_secs) {
			hb_t0 = jiffies;
			INIT_DELAYED_WORK(&hb_work, heartbeat);
			schedule_delayed_work(&hb_work, HZ);
		}
	}

	if (register_fb) {
		pdata.width = width;
		pdata.height = height;
		pdata.stride = stride;
		pdata.format = format;
		pdev = platform_device_register_resndata(NULL, "simple-framebuffer", PLATFORM_DEVID_AUTO, &res, 1, &pdata,
							 sizeof(pdata));
		if (IS_ERR(pdev)) {
			int err = PTR_ERR(pdev);

			pdev = NULL;
			pr_err("tb330fu_fb: registering simple-framebuffer failed: %d\n", err);
			if (map) {
				if (hb_secs)
					cancel_delayed_work_sync(&hb_work);
				iounmap(map);
				map = NULL;
			}
			return err;
		}
		pr_info("tb330fu_fb: simple-framebuffer registered (%pR)\n", &res);
	}
	dbg_init();
	return 0;
}

static void __exit fb_exit(void)
{
	dbg_exit();
	if (hb_secs && map)
		cancel_delayed_work_sync(&hb_work);
	if (pdev)
		platform_device_unregister(pdev);
	if (map)
		iounmap(map);
}

module_init(fb_init);
module_exit(fb_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TB330FU: paint a test pattern into LK's framebuffer and register it as simple-framebuffer for simpledrm");
