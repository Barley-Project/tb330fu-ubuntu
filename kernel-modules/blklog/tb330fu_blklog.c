// SPDX-License-Identifier: GPL-2.0
/*
 * tb330fu_blklog - keep a persistent copy of the kernel log in an UNUSED part of the expdb partition.
 *
 * Why: while Ubuntu runs on the tablet nothing is visible, and MediaTek's expdb only receives the kernel log when the kernel panics AND the
 * device then boots into LK once more (LK's "kedump" copies the panic console).  Cutting the power earlier loses the whole log (this
 * happened with earlier boot-test images).  This module writes a snapshot of the kernel ring buffer straight into the expdb partition every few
 * seconds, so that the log survives any power-off; the user simply dumps expdb as before and the snapshot is found at a fixed offset.
 *
 * Safety:
 *   - the partition is given by stage1 (major:minor of the GPT entry named "expdb"; p6, 20 MiB, on this device);
 *   - the first 4 bytes of the partition must be the expdb magic ad de e0 ae, otherwise nothing is written;
 *   - only the window [off, off + size) is ever written (default 0x1120000 + 0xD8000: the unused gap between MediaTek's kedump items and
 *     the LK/PL log ring at 0x1200000, all zero in every dump seen so far);
 *   - the window is used only if it is entirely zero, or if its slot headers already carry this module's magic (previous run);
 *   - it stops after stop_s seconds.
 *
 * Window layout (two slots written alternately, the older one stays valid while the newer one is written):
 *   slot 0 at off, slot 1 at off + SLOT_SIZE.  Slot = 4 KiB header page + data.
 *   data = [F section: F_MAX bytes, "important" lines from the START of the log (module output, errors, drm, systemd progress)]
 *          [T section: the newest t_len bytes of the whole log]
 *   header (little endian): magic "TB330LG1", version, seq, slot, f_len, t_len, f_max, data_crc (crc32_le(0, data, f_max + t_len)),
 *   uptime_ms, kmsg records seen, hcrc (crc32_le over the preceding header bytes).  Pick the valid slot with the larger seq.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/workqueue.h>
#include <linux/string.h>
#include <linux/crc32.h>
#include <linux/vmalloc.h>
#include <linux/kmsg_dump.h>
#include <linux/sched/clock.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/kernel.h>

static char *devt;
module_param(devt, charp, 0444);
MODULE_PARM_DESC(devt, "MAJOR:MINOR of the expdb partition");

static unsigned long off = 0x1120000UL;
module_param(off, ulong, 0444);
MODULE_PARM_DESC(off, "byte offset of the log window inside the partition");

static unsigned long size = 0xD8000UL;
module_param(size, ulong, 0444);
MODULE_PARM_DESC(size, "size of the log window (two slots)");

static unsigned int interval_ms = 5000;
module_param(interval_ms, uint, 0444);
MODULE_PARM_DESC(interval_ms, "milliseconds between two snapshots");

static unsigned int stop_s = 1500;
module_param(stop_s, uint, 0444);
MODULE_PARM_DESC(stop_s, "stop writing this many seconds after load (0 = never)");

#define HDR_SIZE	4096u
#define SLOT_SIZE	0x6C000u
#define F_MAX		(156u * 1024u)
#define T_MAX		(SLOT_SIZE - HDR_SIZE - F_MAX)
#define MAGIC		"TB330LG1"
#define SCRATCH		(64u * 1024u)
#define FAST_SNAPSHOTS	20u

struct blklog_hdr {
	char magic[8];
	u32 version;
	u32 seq;
	u32 slot;
	u32 f_len;
	u32 t_len;
	u32 f_max;
	u32 data_crc;
	u32 uptime_ms;
	u32 records;
	u32 hcrc;
};

static char *vbuf;		/* F_MAX + T_MAX (+ one page) */
static char *hbuf;		/* one page: the header, also the scratch for checks */
static u32 seq;
static u64 t_load_ns;
static struct delayed_work blklog_work;
static bool checked, dead;
static unsigned int nwritten;

/* ------------------------------------------------------------------------------------------------ block I/O (kernel only) */
#ifndef TB_HOSTTEST
static struct block_device *bdev;

/* off and len are multiples of PAGE_SIZE, buf is page aligned vmalloc memory */
static int io_rw(bool write, u64 byte_off, void *buf, size_t len, bool fua)
{
	size_t done = 0;

	while (done < len) {
		size_t chunk = min_t(size_t, len - done, 128 * PAGE_SIZE);
		unsigned int nr = DIV_ROUND_UP(chunk, PAGE_SIZE), i;
		struct bio *bio;
		int ret;

		bio = bio_alloc(bdev, nr, write ? (REQ_OP_WRITE | REQ_SYNC | (fua ? REQ_FUA : 0)) : REQ_OP_READ, GFP_KERNEL);
		if (!bio)
			return -ENOMEM;
		bio->bi_iter.bi_sector = (byte_off + done) >> SECTOR_SHIFT;
		for (i = 0; i < nr; i++) {
			struct page *pg = vmalloc_to_page((char *)buf + done + (size_t)i * PAGE_SIZE);

			if (!pg || bio_add_page(bio, pg, PAGE_SIZE, 0) != PAGE_SIZE) {
				bio_put(bio);
				return -EIO;
			}
		}
		ret = submit_bio_wait(bio);
		bio_put(bio);
		if (ret)
			return ret;
		done += chunk;
	}
	return 0;
}

static void io_flush(void)
{
	blkdev_issue_flush(bdev);
}

static u64 io_size(void)
{
	return (u64)bdev_nr_bytes(bdev);
}
#endif /* !TB_HOSTTEST */

/* ------------------------------------------------------------------------------------------------ snapshot */
static const char *const keywords[] = {
	"tb330fu", "TB330FU", "simpledrm", "[drm", "drm:", "himax", "HXTP", "hx83", "sddm", "kwin", "plasma", "Xorg", "Failed", "failed",
	"error", "ERROR", "Error", "BUG", "WARNING", "Oops", "panic", "segfault", "dumped core", "denied", "Started ", "Reached target",
	"Listening", "login", "session",
};

static bool line_important(const char *s)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(keywords); i++)
		if (strstr(s, keywords[i]))
			return true;
	return false;
}

static void build_snapshot(size_t *f_len, size_t *t_len, u32 *records)
{
	struct kmsg_dump_iter iter;
	char line[512];
	size_t l = 0, fo = 0;
	u32 n = 0;

	memset(vbuf, 0, F_MAX);
	kmsg_dump_rewind(&iter);
	while (kmsg_dump_get_line(&iter, false, line, sizeof(line) - 1, &l)) {
		n++;
		line[l] = 0;
		if (l && fo + l <= F_MAX && line_important(line)) {
			memcpy(vbuf + fo, line, l);
			fo += l;
		}
	}
	kmsg_dump_rewind(&iter);
	l = 0;
	if (!kmsg_dump_get_buffer(&iter, false, vbuf + F_MAX, T_MAX, &l))
		l = 0;
	*f_len = fo;
	*t_len = l;
	*records = n;
}

static void hdr_finish(struct blklog_hdr *h)
{
	h->hcrc = crc32_le(0, (const u8 *)h, offsetof(struct blklog_hdr, hcrc));
}

static bool hdr_valid(const struct blklog_hdr *h)
{
	return !memcmp(h->magic, MAGIC, 8) && h->version == 1 && h->hcrc == crc32_le(0, (const u8 *)h, offsetof(struct blklog_hdr, hcrc));
}

static int write_snapshot(void)
{
	struct blklog_hdr *h = (struct blklog_hdr *)hbuf;
	size_t f_len, t_len, data_len;
	u32 records, slot;
	u64 base;
	int ret;

	build_snapshot(&f_len, &t_len, &records);
	data_len = F_MAX + round_up(t_len, PAGE_SIZE);
	if (t_len % PAGE_SIZE)
		memset(vbuf + F_MAX + t_len, 0, PAGE_SIZE - t_len % PAGE_SIZE);
	seq++;
	slot = seq & 1;
	base = (u64)off + (u64)slot * SLOT_SIZE;
	ret = io_rw(true, base + HDR_SIZE, vbuf, data_len, false);
	if (ret)
		return ret;
	memset(hbuf, 0, PAGE_SIZE);
	memcpy(h->magic, MAGIC, 8);
	h->version = 1;
	h->seq = seq;
	h->slot = slot;
	h->f_len = f_len;
	h->t_len = t_len;
	h->f_max = F_MAX;
	h->data_crc = crc32_le(0, (const u8 *)vbuf, F_MAX + t_len);
	h->uptime_ms = div_u64(sched_clock(), 1000000);
	h->records = records;
	hdr_finish(h);
	ret = io_rw(true, base, hbuf, PAGE_SIZE, true);
	if (ret)
		return ret;
	io_flush();
	return 0;
}

/* returns 0 when the window may be used */
static int check_window(void)
{
	struct blklog_hdr *h = (struct blklog_hdr *)hbuf;
	bool ours = false;
	u64 o, psize = io_size();
	u32 s, max_seq = 0;
	int ret;

	if (size < 2ul * SLOT_SIZE || (off & (PAGE_SIZE - 1)) || (u64)off + size > psize) {
		pr_err("tb330fu_blklog: window %#lx+%#lx does not fit the %#llx byte partition (or is too small / unaligned), nothing written\n", off,
		       size, (unsigned long long)psize);
		return -EINVAL;
	}
	ret = io_rw(false, 0, hbuf, PAGE_SIZE, false);
	if (ret) {
		pr_err("tb330fu_blklog: reading the partition start failed: %d\n", ret);
		return ret;
	}
	if ((u8)hbuf[0] != 0xad || (u8)hbuf[1] != 0xde || (u8)hbuf[2] != 0xe0 || (u8)hbuf[3] != 0xae) {
		pr_err("tb330fu_blklog: partition does not start with the expdb magic (%02x %02x %02x %02x), nothing written\n", (u8)hbuf[0],
		       (u8)hbuf[1], (u8)hbuf[2], (u8)hbuf[3]);
		return -ENODEV;
	}
	for (s = 0; s < 2; s++) {
		ret = io_rw(false, (u64)off + (u64)s * SLOT_SIZE, hbuf, PAGE_SIZE, false);
		if (ret)
			return ret;
		if (hdr_valid(h)) {
			ours = true;
			if (h->seq > max_seq)
				max_seq = h->seq;
		}
	}
	if (!ours) {
		for (o = 0; o < size; o += SCRATCH) {
			size_t n = min_t(u64, SCRATCH, size - o);

			ret = io_rw(false, (u64)off + o, vbuf, n, false);
			if (ret)
				return ret;
			if (memchr_inv(vbuf, 0, n)) {
				pr_err("tb330fu_blklog: the window is not empty (non-zero data at %#llx) and has no tb330fu header, nothing written\n",
				       (unsigned long long)((u64)off + o));
				return -EBUSY;
			}
		}
	}
	seq = max_seq;
	pr_info("tb330fu_blklog: window %#lx+%#lx in the %#llx byte expdb partition is %s, continuing at seq %u\n", off, size, (unsigned long long)psize,
		ours ? "ours from an earlier run" : "empty", seq + 1);
	return 0;
}

static void blklog_fn(struct work_struct *w)
{
	int ret;

	if (dead)
		return;
	if (!checked) {
		ret = check_window();
		if (ret) {
			dead = true;
			return;
		}
		checked = true;
	}
	ret = write_snapshot();
	if (ret) {
		pr_err("tb330fu_blklog: snapshot write failed: %d, giving up\n", ret);
		dead = true;
		return;
	}
	if (!(nwritten++ % 12))
		pr_info("tb330fu_blklog: snapshot %u written (seq %u)\n", nwritten, seq);
	if (!stop_s || div_u64(sched_clock() - t_load_ns, 1000000000) < stop_s) {
		/* the first snapshots come fast (1 s) so that a hang shortly after load still leaves a nearly complete log */
		queue_delayed_work(system_long_wq, &blklog_work, msecs_to_jiffies(nwritten < FAST_SNAPSHOTS ? 1000 : interval_ms));
	} else {
		pr_info("tb330fu_blklog: stop_s=%u reached, last snapshot seq %u\n", stop_s, seq);
	}
}

static int parse_devt(const char *s, unsigned int *maj, unsigned int *min)
{
	char tmp[16];
	const char *c = strchr(s, ':');
	size_t n;

	if (!c)
		return -EINVAL;
	n = c - s;
	if (!n || n >= sizeof(tmp))
		return -EINVAL;
	memcpy(tmp, s, n);
	tmp[n] = 0;
	if (kstrtouint(tmp, 10, maj) || kstrtouint(c + 1, 10, min))
		return -EINVAL;
	return 0;
}

#ifndef TB_HOSTTEST
static int __init blklog_init(void)
{
	unsigned int maj, min;
	int ret;

	if (!devt || !*devt || parse_devt(devt, &maj, &min)) {
		pr_err("tb330fu_blklog: missing or bad devt= '%s', nothing to do\n", devt ? devt : "");
		return -EINVAL;
	}
	vbuf = vmalloc(F_MAX + T_MAX + PAGE_SIZE);
	hbuf = vmalloc(PAGE_SIZE);
	if (!vbuf || !hbuf) {
		ret = -ENOMEM;
		goto err_free;
	}
	bdev = blkdev_get_by_dev(MKDEV(maj, min), BLK_OPEN_READ | BLK_OPEN_WRITE, NULL, NULL);
	if (IS_ERR(bdev)) {
		ret = PTR_ERR(bdev);
		pr_err("tb330fu_blklog: cannot open %u:%u: %d\n", maj, min, ret);
		goto err_free;
	}
	t_load_ns = sched_clock();
	pr_info("tb330fu_blklog: opened %u:%u (%llu bytes), window %#lx+%#lx, snapshot every %ums for %us\n", maj, min, (unsigned long long)bdev_nr_bytes(bdev), off,
		size, interval_ms, stop_s);
	/* the safety checks run here, so that insmod FAILS (and stage1 arms its planned-panic fallback) if the partition or the window is unsuitable */
	ret = check_window();
	if (ret) {
		blkdev_put(bdev, NULL);
		goto err_free;
	}
	checked = true;
	INIT_DELAYED_WORK(&blklog_work, blklog_fn);
	queue_delayed_work(system_long_wq, &blklog_work, msecs_to_jiffies(1000));
	return 0;

err_free:
	vfree(vbuf);
	vfree(hbuf);
	vbuf = hbuf = NULL;
	return ret;
}

static void __exit blklog_exit(void)
{
	cancel_delayed_work_sync(&blklog_work);
	if (!dead && checked)
		write_snapshot();
	blkdev_put(bdev, NULL);
	vfree(vbuf);
	vfree(hbuf);
}

module_init(blklog_init);
module_exit(blklog_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TB330FU: persistent kernel-log snapshots in an unused window of the expdb partition");
#endif /* !TB_HOSTTEST */
