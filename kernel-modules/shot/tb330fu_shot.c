// SPDX-License-Identifier: GPL-2.0
/*
 * tb330fu_shot - screenshot of what the panel scans, for the remote debug loop (loaded on demand over ssh, never at boot).
 *
 * simpledrm flips OVL0 layer 0 between two or three buffers of LK's framebuffer region (0x8E8000 apart), so the scanned buffer changes with every frame.
 *   /sys/kernel/tb330fu_shot/front : "<0|1|2> addr=<OVL0 layer 0 address register>"  which buffer the display engine was pointed at when read
 *   /sys/kernel/tb330fu_shot/cur   : raw bytes of THAT buffer (stride x height, a8r8g8b8, native portrait); the buffer is latched when offset 0 is read
 *   /sys/kernel/tb330fu_shot/buf0, buf1, buf2 : the buffers
 * Read-only: nothing is written to the display hardware.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/io.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>

#define NBUF 3

static unsigned long fb_pa[NBUF] = { 0x7c8e0000UL, 0x7d1c8000UL, 0x7dab0000UL };
module_param_named(fb0_pa, fb_pa[0], ulong, 0444);
module_param_named(fb1_pa, fb_pa[1], ulong, 0444);
module_param_named(fb2_pa, fb_pa[2], ulong, 0444);
static unsigned int width = 1200;
module_param(width, uint, 0444);
static unsigned int height = 1920;
module_param(height, uint, 0444);
static unsigned int stride = 4864;
module_param(stride, uint, 0444);
static unsigned long ovl_reg_pa = 0x1400bf40UL;
module_param(ovl_reg_pa, ulong, 0444);

static void __iomem *bufs[NBUF];
static void __iomem *ovl_page;
static void __iomem *ovl;
static struct kobject *shot_kobj;
static int latched;

static u32 reg_now(void)
{
	return readl_relaxed(ovl);
}

static int front_of(u32 a)
{
	unsigned long base = (unsigned long)a - ((unsigned long)stride * (height - 1) + 4ul * width + 3);
	int i;

	for (i = 1; i < NBUF; i++)
		if (base == fb_pa[i])
			return i;
	return 0;
}

static ssize_t shot_read(struct file *filp, struct kobject *kobj, struct bin_attribute *attr, char *buf, loff_t off, size_t count)
{
	size_t total = (size_t)stride * height;
	int which = (int)(long)attr->private;

	if (off < 0 || (size_t)off >= total)
		return 0;
	if (count > total - (size_t)off)
		count = total - (size_t)off;
	if (which == NBUF) {
		if (off == 0)
			latched = front_of(reg_now());
		which = latched;
	}
	memcpy_fromio(buf, bufs[which] + off, count);
	return count;
}

static struct bin_attribute cur_attr = { .attr = { .name = "cur", .mode = 0400 }, .read = shot_read, .private = (void *)(long)NBUF };
static struct bin_attribute buf_attr[NBUF] = {
	{ .attr = { .name = "buf0", .mode = 0400 }, .read = shot_read, .private = (void *)0L },
	{ .attr = { .name = "buf1", .mode = 0400 }, .read = shot_read, .private = (void *)1L },
	{ .attr = { .name = "buf2", .mode = 0400 }, .read = shot_read, .private = (void *)2L },
};

static ssize_t front_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	u32 a = reg_now();

	return sysfs_emit(buf, "%d addr=%08x stride=%u width=%u height=%u\n", front_of(a), a, stride, width, height);
}

static struct kobj_attribute front_attr = __ATTR_RO(front);

static int __init shot_init(void)
{
	size_t total = (size_t)stride * height;
	int i;

	for (i = 0; i < NBUF; i++) {
		bufs[i] = ioremap_wc(fb_pa[i], total);
		if (!bufs[i])
			goto err;
	}
	ovl_page = ioremap(ovl_reg_pa & PAGE_MASK, PAGE_SIZE);
	if (!ovl_page)
		goto err;
	ovl = ovl_page + (ovl_reg_pa & ~PAGE_MASK);
	shot_kobj = kobject_create_and_add("tb330fu_shot", kernel_kobj);
	if (!shot_kobj)
		goto err;
	cur_attr.size = total;
	if (sysfs_create_bin_file(shot_kobj, &cur_attr))
		goto err;
	for (i = 0; i < NBUF; i++) {
		buf_attr[i].size = total;
		if (sysfs_create_bin_file(shot_kobj, &buf_attr[i]))
			goto err;
	}
	if (sysfs_create_file(shot_kobj, &front_attr.attr))
		goto err;
	pr_info("tb330fu_shot: /sys/kernel/tb330fu_shot/{cur,buf0,buf1,buf2,front}: buffers %#lx %#lx %#lx, OVL address register %#lx = %08x\n", fb_pa[0],
		fb_pa[1], fb_pa[2], ovl_reg_pa, reg_now());
	return 0;
err:
	pr_err("tb330fu_shot: setup failed\n");
	if (shot_kobj)
		kobject_put(shot_kobj);
	if (ovl_page)
		iounmap(ovl_page);
	for (i = 0; i < NBUF; i++)
		if (bufs[i])
			iounmap(bufs[i]);
	return -ENOMEM;
}

static void __exit shot_exit(void)
{
	int i;

	sysfs_remove_bin_file(shot_kobj, &cur_attr);
	for (i = 0; i < NBUF; i++)
		sysfs_remove_bin_file(shot_kobj, &buf_attr[i]);
	sysfs_remove_file(shot_kobj, &front_attr.attr);
	kobject_put(shot_kobj);
	iounmap(ovl_page);
	for (i = 0; i < NBUF; i++)
		iounmap(bufs[i]);
}

module_init(shot_init);
module_exit(shot_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TB330FU: screenshot of the scanned framebuffer");
