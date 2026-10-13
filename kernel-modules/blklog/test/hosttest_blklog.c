/*
 * Host-side test of tb330fu_blklog.c: the module source is #included with TB_HOSTTEST and a small shim of the kernel API.  The block device
 * is a 20 MiB memory image (expdb magic at offset 0), kmsg is a fake line list.  The produced window is also written to a file so that
 * 340_parse_blklog.py can be run against it.
 *
 * build: gcc -O1 -Wall -Wno-unused-function -Ifakeinc -o hosttest_blklog 341_hosttest_blklog.c
 */
#define _GNU_SOURCE 1
#define TB_HOSTTEST 1
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <stddef.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define __init
#define __exit
#define pr_info(...) printf(__VA_ARGS__)
#define pr_err(...) printf(__VA_ARGS__)
#define module_param(a, b, c)
#define MODULE_PARM_DESC(a, b)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PAGE_SIZE 4096ul
#define round_up(x, y) ((((x) + (y) - 1) / (y)) * (y))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define msecs_to_jiffies(x) (x)
#define div_u64(a, b) ((u64)(a) / (b))

struct work_struct { int unused; };
struct delayed_work { struct work_struct work; void (*fn)(struct work_struct *); };
#define INIT_DELAYED_WORK(w, f) ((w)->fn = (f))
static int wq_obj;
static int *system_long_wq = &wq_obj;
static int requeued;
static unsigned long last_delay;
static void queue_delayed_work(int *wq, struct delayed_work *w, unsigned long d) { (void)wq; (void)w; last_delay = d; requeued++; }

static void *vmalloc(size_t n) { return aligned_alloc(4096, (n + 4095) / 4096 * 4096); }
static void vfree(void *p) { free(p); }
static u32 crc32_le(u32 crc, const u8 *p, size_t len)
{
	while (len--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (-(crc & 1)));
	}
	return crc;
}
static void *memchr_inv(const void *s, int c, size_t n)
{
	const u8 *p = s;
	for (size_t i = 0; i < n; i++)
		if (p[i] != (u8)c)
			return (void *)(p + i);
	return NULL;
}
static u64 fake_clock = 100ull * 1000000000ull;
static u64 sched_clock(void) { return fake_clock += 2000000000ull; }	/* 2 s per call */
static int kstrtouint(const char *s, unsigned int base, unsigned int *res)
{
	char *e;
	unsigned long v = strtoul(s, &e, base);
	if (e == s || *e)
		return -EINVAL;
	*res = (unsigned int)v;
	return 0;
}

/* ---- fake kmsg */
struct kmsg_dump_iter { unsigned int cur; };
#define MAXLINES 200000
static char *klines[MAXLINES];
static int nklines;
static void kmsg_dump_rewind(struct kmsg_dump_iter *it) { it->cur = 0; }
static bool kmsg_dump_get_line(struct kmsg_dump_iter *it, bool syslog, char *line, size_t size, size_t *len)
{
	(void)syslog;
	if ((int)it->cur >= nklines)
		return false;
	size_t l = strlen(klines[it->cur]);
	if (l > size)
		l = size;
	memcpy(line, klines[it->cur], l);
	*len = l;
	it->cur++;
	return true;
}
static bool kmsg_dump_get_buffer(struct kmsg_dump_iter *it, bool syslog, char *buf, size_t size, size_t *len)
{
	(void)syslog;
	(void)it;
	/* newest whole lines that fit */
	size_t used = 0;
	int first = nklines;
	while (first > 0 && used + strlen(klines[first - 1]) <= size - 1) {
		used += strlen(klines[first - 1]);
		first--;
	}
	size_t o = 0;
	for (int i = first; i < nklines; i++) {
		size_t l = strlen(klines[i]);
		memcpy(buf + o, klines[i], l);
		o += l;
	}
	*len = o;
	return nklines > 0;
}

/* ---- fake block device */
#define PART_SIZE (20ull * 1024 * 1024)
static u8 *part;
static int io_fail_after = -1, io_writes;
static int io_rw(bool write, u64 byte_off, void *buf, size_t len, bool fua)
{
	(void)fua;
	if (byte_off + len > PART_SIZE)
		return -EIO;
	if (write) {
		if (io_fail_after >= 0 && io_writes >= io_fail_after)
			return -EIO;
		io_writes++;
		memcpy(part + byte_off, buf, len);
	} else {
		memcpy(buf, part + byte_off, len);
	}
	return 0;
}
static void io_flush(void) {}
static u64 io_size(void) { return PART_SIZE; }

#include "../tb330fu_blklog.c"

static int fails;
#define CHECK(name, cond) do { printf("%s  %s\n", (cond) ? "PASS" : "FAIL", name); if (!(cond)) fails++; } while (0)

static void new_part(bool magic, bool dirty_window)
{
	if (!part)
		part = calloc(1, PART_SIZE);
	memset(part, 0, PART_SIZE);
	if (magic) {
		part[0] = 0xad; part[1] = 0xde; part[2] = 0xe0; part[3] = 0xae;
	}
	if (dirty_window)
		part[0x1120000 + 0x30000] = 0x55;
	vbuf = hbuf = NULL;
	vbuf = vmalloc(F_MAX + T_MAX + PAGE_SIZE);
	hbuf = vmalloc(PAGE_SIZE);
	seq = 0; checked = dead = false; nwritten = 0; requeued = 0; io_writes = 0; io_fail_after = -1;
	t_load_ns = sched_clock();
	INIT_DELAYED_WORK(&blklog_work, blklog_fn);
}
static void clear_log(void)
{
	for (int i = 0; i < nklines; i++)
		free(klines[i]);
	nklines = 0;
}
static void addline(const char *fmt, int n)
{
	char tmp[600];
	snprintf(tmp, sizeof(tmp), fmt, n);
	klines[nklines++] = strdup(tmp);
}
static const u8 *slot_hdr(int s) { return part + off + (size_t)s * SLOT_SIZE; }
static struct blklog_hdr rd_hdr(int s)
{
	struct blklog_hdr h;
	memcpy(&h, slot_hdr(s), sizeof(h));
	return h;
}

int main(void)
{
	/* a log of 3000 lines, a few important */
	for (int i = 0; i < 3000; i++)
		addline(i % 100 == 7 ? "[%10d] tb330fu_ovl: something important\n" : "[%10d] boring line of the kernel boot log\n", i);

	printf("==== 1. fresh partition, empty window\n");
	new_part(true, false);
	blklog_fn(NULL);
	CHECK("not dead, checked", !dead && checked);
	CHECK("slot 1 written first (seq 1 -> slot 1)", ((struct blklog_hdr *)slot_hdr(1))->seq == 1);
	blklog_fn(NULL);
	CHECK("second snapshot goes to slot 0 (seq 2)", ((struct blklog_hdr *)slot_hdr(0))->seq == 2);
	{
		struct blklog_hdr h0 = rd_hdr(0), h1 = rd_hdr(1);
		CHECK("both headers valid", hdr_valid(&h0) && hdr_valid(&h1));
		CHECK("F section holds the important lines only", h0.f_len > 0 && memmem(part + off + HDR_SIZE, h0.f_len, "boring", 6) == NULL);
		const u8 *d0 = part + off + HDR_SIZE;
		CHECK("F section starts with the first important line", memmem(d0, 64, "tb330fu_ovl", 11) != NULL);
		CHECK("data crc matches", h0.data_crc == crc32_le(0, d0, F_MAX + h0.t_len));
		CHECK("T section ends with the newest line", memmem(d0 + F_MAX + h0.t_len - 60, 60, "[      2999]", 12) != NULL);
		CHECK("records counted", h0.records == 3000);
		CHECK("requeued for the next interval", requeued == 2);
		CHECK("early snapshots are requeued after 1 s", last_delay == 1000);
	}
	CHECK("nothing outside the window changed", part[0] == 0xad && !memchr_inv(part + 4, 0, 0x1120000 - 4) &&
	      !memchr_inv(part + 0x1120000 + 2 * SLOT_SIZE, 0, PART_SIZE - 0x1120000 - 2 * SLOT_SIZE));
	{
		FILE *f = fopen("blklog_window.bin", "wb");
		fwrite(part, 1, PART_SIZE, f);
		fclose(f);
	}

	{
		/* after FAST_SNAPSHOTS the interval switches to interval_ms */
		new_part(true, false);
		for (int i = 0; i < 25; i++)
			blklog_fn(NULL);
		CHECK("late snapshots are requeued after interval_ms", last_delay == interval_ms && nwritten == 25);
	}

	printf("\n==== 2. window with foreign data and no header: refuse\n");
	new_part(true, true);
	blklog_fn(NULL);
	CHECK("dead, zero writes", dead && io_writes == 0);

	printf("\n==== 3. partition without the expdb magic: refuse\n");
	new_part(false, false);
	blklog_fn(NULL);
	CHECK("dead, zero writes", dead && io_writes == 0);

	printf("\n==== 4. second run continues the sequence\n");
	new_part(true, false);
	blklog_fn(NULL); blklog_fn(NULL); blklog_fn(NULL);
	u32 last = ((struct blklog_hdr *)slot_hdr(1))->seq;	/* seq 3 -> slot 1 */
	CHECK("three snapshots, seq 3 in slot 1", last == 3);
	seq = 0; checked = dead = false;	/* a new module load */
	blklog_fn(NULL);
	CHECK("continues at seq 4 (slot 0)", ((struct blklog_hdr *)slot_hdr(0))->seq == 4);

	printf("\n==== 5. huge log: T section is capped, the newest lines survive; F capped\n");
	clear_log();
	for (int i = 0; i < 60000; i++)
		addline(i % 3 == 0 ? "[%10d] kwin_wayland: failed to do something, an important line of padding text padding\n" : "[%10d] boring boring boring boring boring boring boring boring\n", i);
	new_part(true, false);
	blklog_fn(NULL);
	{
		struct blklog_hdr h = rd_hdr(1);
		CHECK("header valid", hdr_valid(&h));
		CHECK("F full but within F_MAX", h.f_len <= F_MAX && h.f_len > F_MAX - 200);
		CHECK("T within T_MAX and page-padded zero tail", h.t_len <= T_MAX && h.t_len > T_MAX - 200);
		const u8 *d = part + off + SLOT_SIZE + HDR_SIZE;
		CHECK("last T line is the last log line", memmem(d + F_MAX + h.t_len - 90, 90, "[     59999]", 12) != NULL);
	}

	printf("\n==== 6. write failure: gives up cleanly\n");
	new_part(true, false);
	io_fail_after = 1;	/* the data write works, the header write fails */
	blklog_fn(NULL);
	CHECK("dead after the failed write", dead);

	printf("\n==== 7. window outside the partition\n");
	new_part(true, false);
	off = 0x1300000;	/* + 0xD8000 > 20 MiB? 0x1300000 = 19 MiB, + 864 KiB = 19.84 MiB: fits; use a bigger one */
	off = 0x1380000;
	blklog_fn(NULL);
	CHECK("refused", dead && io_writes == 0);
	off = 0x1120000;

	printf("\n==== 8. devt parser\n");
	unsigned int a = 0, b = 0;
	CHECK("259:6", parse_devt("259:6", &a, &b) == 0 && a == 259 && b == 6);
	CHECK("rejects garbage", parse_devt("259", &a, &b) != 0 && parse_devt(":6", &a, &b) != 0 && parse_devt("x:6", &a, &b) != 0 && parse_devt("259:", &a, &b) != 0);

	printf("\n%d check(s) failed\n", fails);
	return fails ? 1 : 0;
}
