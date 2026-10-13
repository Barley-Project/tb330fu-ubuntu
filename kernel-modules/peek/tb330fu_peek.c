// SPDX-License-Identifier: GPL-2.0
/*
 * tb330fu_peek - read / write physical registers from user space (remote hardware bring-up tool; loaded on demand, never at boot).
 *   /sys/kernel/tb330fu_peek/addr   hex physical address (4-byte aligned), read/write
 *   /sys/kernel/tb330fu_peek/val    read: 32 bit register at addr; write: store the value (hex) into the register at addr
 *   /sys/kernel/tb330fu_peek/dump   write "<addr> <count>" then read: <count> consecutive 32 bit words starting at addr, 8 per line
 * Every access maps the page, accesses it and unmaps it again; nothing is cached.  Root only (mode 0600).
 * DANGEROUS by design: reading a clock-gated block can hang the bus, a wrong write can reset the SoC.
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/kernel.h>
#include <linux/slab.h>

static unsigned long cur_addr;
static unsigned long dump_addr;
static unsigned long dump_count = 16;
static struct kobject *peek_kobj;

static ssize_t addr_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%#lx\n", cur_addr);
}

static ssize_t addr_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned long v;

	if (kstrtoul(buf, 0, &v))
		return -EINVAL;
	if (v & 3)
		return -EINVAL;
	cur_addr = v;
	return count;
}

static ssize_t val_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	void __iomem *p = ioremap(cur_addr & PAGE_MASK, PAGE_SIZE);
	u32 v;

	if (!p)
		return -ENOMEM;
	v = readl(p + (cur_addr & ~PAGE_MASK));
	iounmap(p);
	return sysfs_emit(buf, "%08x\n", v);
}

static ssize_t val_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	void __iomem *p;
	unsigned long v;

	if (kstrtoul(buf, 0, &v))
		return -EINVAL;
	p = ioremap(cur_addr & PAGE_MASK, PAGE_SIZE);
	if (!p)
		return -ENOMEM;
	writel((u32)v, p + (cur_addr & ~PAGE_MASK));
	iounmap(p);
	return count;
}

static ssize_t dump_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	unsigned long n = min_t(unsigned long, dump_count, 256), i;
	ssize_t len = 0;
	void __iomem *p;

	p = ioremap(dump_addr & PAGE_MASK, PAGE_SIZE * 2);
	if (!p)
		return -ENOMEM;
	for (i = 0; i < n; i++) {
		unsigned long off = (dump_addr & ~PAGE_MASK) + 4 * i;

		if (off >= PAGE_SIZE * 2)
			break;
		if (i % 8 == 0)
			len += sysfs_emit_at(buf, len, "%s%08lx:", i ? "\n" : "", dump_addr + 4 * i);
		len += sysfs_emit_at(buf, len, " %08x", readl(p + off));
	}
	len += sysfs_emit_at(buf, len, "\n");
	iounmap(p);
	return len;
}

static ssize_t dump_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned long a, c = 16;

	if (sscanf(buf, "%lx %lu", &a, &c) < 1)
		return -EINVAL;
	if (a & 3)
		return -EINVAL;
	dump_addr = a;
	dump_count = c;
	return count;
}

static struct kobj_attribute addr_attr = __ATTR(addr, 0600, addr_show, addr_store);
static struct kobj_attribute val_attr = __ATTR(val, 0600, val_show, val_store);
static struct kobj_attribute dump_attr = __ATTR(dump, 0600, dump_show, dump_store);

static int __init peek_init(void)
{
	peek_kobj = kobject_create_and_add("tb330fu_peek", kernel_kobj);
	if (!peek_kobj)
		return -ENOMEM;
	if (sysfs_create_file(peek_kobj, &addr_attr.attr) || sysfs_create_file(peek_kobj, &val_attr.attr) || sysfs_create_file(peek_kobj, &dump_attr.attr)) {
		kobject_put(peek_kobj);
		return -ENOMEM;
	}
	pr_info("tb330fu_peek: /sys/kernel/tb330fu_peek/{addr,val,dump} ready\n");
	return 0;
}

static void __exit peek_exit(void)
{
	sysfs_remove_file(peek_kobj, &addr_attr.attr);
	sysfs_remove_file(peek_kobj, &val_attr.attr);
	sysfs_remove_file(peek_kobj, &dump_attr.attr);
	kobject_put(peek_kobj);
}

module_init(peek_init);
module_exit(peek_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TB330FU: physical register peek/poke over sysfs (remote bring-up tool)");
