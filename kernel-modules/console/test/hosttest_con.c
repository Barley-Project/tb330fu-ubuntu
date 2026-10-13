/*
 * Host-side test of kernel-modules/console/tb330fu_con.c: the module source is #included with a shim of the kernel API.  The framebuffer is
 * a host buffer (stride 4864), the log lines come from a text file given on the command line.  The test checks the text grid logic,
 * the noise filter, the synchronous panic path, the stop handshake and writes the rendered framebuffer to a raw file so that it can be
 * looked at (convert with 393_raw2png.py).
 *
 * build: gcc -O1 -Wall -Wno-unused-function -Wno-pointer-arith -Ifakeinc -I.. -o hosttest_con hosttest_con.c
 * usage: hosttest_con <log.txt> <out.raw> [rotate [font [quiet]]]
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define __init
#define __exit
#define __iomem
#define pr_info(...) printf(__VA_ARGS__)
#define pr_err(...) printf(__VA_ARGS__)
#define module_param(a, b, c)
#define module_param_named(a, b, c, d)
#define MODULE_PARM_DESC(a, b)
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define module_init(x)
#define module_exit(x)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define div_u64(a, b) ((u64)(a) / (b))
#define HZ 250
static unsigned long jiffies;
#define msecs_to_jiffies(x) ((x) / 4)
#define wmb()
#define DEFINE_RAW_SPINLOCK(x) int x
#define raw_spin_lock_irqsave(l, f) ((void)(f), (void)(l))
#define raw_spin_unlock_irqrestore(l, f) ((void)(f), (void)(l))
static int trylock_result = 1;
#define raw_spin_trylock_irqsave(l, f) ((void)(f), (void)(l), trylock_result)
#define vzalloc(n) calloc(1, (n))
#define vmalloc(n) malloc(n)
#define vfree(p) free(p)
#define memcpy_toio(d, s, n) memcpy((void *)(d), (s), (n))
#define readl_relaxed(p) (*(volatile u32 *)(p))
#define writel_relaxed(v, p) (*(volatile u32 *)(p) = (v))
static u8 *fake_fb;
#define ioremap_wc(a, n) ((void *)(fake_fb = calloc(1, (n))))
#define iounmap(p) ((void)0)
static int oops_in_progress;
static u64 sched_clock(void) { return 12345678901ull; }
struct new_utsname { char release[65]; };
static struct new_utsname uts = { "6.6.30-android15-8-gda510ce0fdcf-4k" };
#define utsname() (&uts)
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; void (*fn)(struct work_struct *); };
#define INIT_DELAYED_WORK(w, f) ((w)->fn = (f))
static int wq_obj;
static int *system_long_wq = &wq_obj;
static int requeued;
static void queue_delayed_work(int *wq, struct delayed_work *w, unsigned long d) { (void)wq; (void)w; (void)d; requeued++; }
static int cancel_calls;
static void cancel_delayed_work_sync(struct delayed_work *w) { (void)w; cancel_calls++; }
#define strscpy(d, s, n) (strncpy((d), (s), (n)), (d)[(n) - 1] = 0)
#define CON_PRINTBUFFER 1
#define CON_ENABLED 2
struct console {
	char name[16];
	void (*write)(struct console *, const char *, unsigned int);
	int flags;
	int index;
};
static int registered_calls, unregistered_calls;
static void register_console(struct console *c) { registered_calls++; }
static void unregister_console(struct console *c) { unregistered_calls++; }

#include "../tb330fu_con.c"

static int fails;
#define CHECK(name, cond) do { printf("%s  %s\n", (cond) ? "PASS" : "FAIL", name); if (!(cond)) fails++; } while (0)

static void feed(const char *s) { con_write(&con, s, strlen(s)); }
static const char *row_text(unsigned int r) { static char t[256]; memcpy(t, cells + r * cols, cols); t[cols] = 0; return t; }

int main(int argc, char **argv)
{
	unsigned int exp_cols, exp_rows, exp_hdr;

	if (argc >= 4)
		rotate = (unsigned int)atoi(argv[3]);
	if (argc >= 5)
		font_sel = (unsigned int)atoi(argv[4]);
	if (argc >= 6)
		quiet = (unsigned int)atoi(argv[5]);
	if (con_init())
		return 2;
	printf("---- con_init done: %ux%u cells of %ux%u, %u header rows, %u text rows (rotate %u, font %u, quiet %u)\n", cols, rows, gw, gh, hdr_rows,
	       trows, rotate, font_sel, quiet);
	exp_cols = ((rotate & 1) ? 1920 : 1200) / fonts[font_sel].w;
	exp_rows = ((rotate & 1) ? 1200 : 1920) / fonts[font_sel].h;
	exp_hdr = (16 + 160 + 16 + fonts[font_sel].h - 1) / fonts[font_sel].h;
	CHECK("register_console called once", registered_calls == 1);
	CHECK("geometry follows the font and the rotation", cols == exp_cols && rows == exp_rows && hdr_rows == exp_hdr && trows == exp_rows - exp_hdr);

	/* ---- grid logic */
	feed("abc\r\ndef\n");
	CHECK("line feed moves to the next row", memcmp(row_text(0), "abc", 3) == 0 && memcmp(row_text(1), "def", 3) == 0 && cur_row == 2 && cur_col == 0);
	feed("\tx");
	CHECK("tab advances to a multiple of 8", cells[2 * cols + 8] == 'x');
	{
		char longline[200];
		unsigned int before;

		memset(longline, 'A', 160);
		longline[160] = '\n';
		longline[161] = 0;
		feed("\n");
		before = cur_row;
		feed(longline);
		CHECK("a 160 character line wraps over ceil(160/cols) rows", cur_row == before + DIV_ROUND_UP(160, cols));
	}
	feed("Failed to start polkit.service\n");
	CHECK("an error line is red-classified", rowcol[cur_row - 1] == C_ERR);
	feed("systemd[1]: Started sddm.service - Simple Desktop Display Manager.\n");
	CHECK("a Started line is green-classified", rowcol[cur_row - 1] == C_OK);
	feed("TB330FU INIT: loaded foo.ko\n");
	CHECK("a TB330FU line is cyan-classified", rowcol[cur_row - 1] == C_TB);
	feed("\xe4\xb8\xad" "x\n");
	CHECK("bytes >= 0x80 become '?'", cells[(cur_row - 1) * cols] == '?');

	/* ---- the noise filter */
	{
		u32 h0 = hidden, n0 = nlines;
		unsigned int r0 = cur_row;

		feed("[  105.401082][  T141] [wdk-c] cpu=4 o_k=4 lbit=0x7e\n");
		feed("[  108.912589][   T74] [wdtk] kick watchdog\n");
		feed("[  111.117504][ T2319] tb330fu_blklog: snapshot 37 written (seq 37)\n");
		feed("[   12.000000][    T1] tb330fu_ovl: OVL0 +000: 0 0 0 0 0 0 0 0\n");
		feed("[   13.000000][ T123] tb330fu-devd[123]: tb330fu-devd: coldboot block 7:0\n");
		if (quiet) {
			CHECK("noise lines are counted as hidden and not drawn", hidden == h0 + 5 && nlines == n0 && cur_row == r0);
		} else {
			CHECK("with quiet=0 nothing is hidden", hidden == h0 && nlines == n0 + 5 && cur_row == r0 + 5);
		}
		feed("[  114.280000][    T1] systemd[1]: Starting polkit.service - Authorization Manager...\n");
		CHECK("an ordinary line is always shown", nlines >= n0 + (quiet ? 1 : 6));
	}

	/* ---- scrolling */
	for (unsigned int i = 0; i < trows + 5; i++) {
		char t[40];
		snprintf(t, sizeof(t), "scroll %u\n", i);
		feed(t);
	}
	CHECK("after more lines than rows the cursor sits on the last row", cur_row == trows - 1);
	CHECK("the last printed line is on row trows-2", strncmp(row_text(trows - 2), "scroll", 6) == 0);

	/* ---- the real log */
	if (argc >= 2) {
		FILE *f = fopen(argv[1], "r");
		char line[1100];
		unsigned int n = 0;
		while (f && fgets(line, sizeof(line), f)) {
			feed(line);
			n++;
		}
		if (f)
			fclose(f);
		printf("fed %u log lines (%u shown, %u quiet)\n", n, nlines, hidden);
	}
	render_now();
	CHECK("a frame was rendered into the framebuffer", fake_fb != NULL);
	/* Tux: the pixels of the logo box (16,16)-(176,176) must not all be black */
	{
		unsigned int nonblack = 0;
		for (unsigned int y = 16; y < 16 + 160; y += 4)
			for (unsigned int x = 16; x < 16 + 160; x += 4) {
				unsigned int fx = x, fy = y;

				if (rotate == 1) { fx = 1199 - y; fy = x; }
				else if (rotate == 2) { fx = 1199 - x; fy = 1919 - y; }
				else if (rotate == 3) { fx = y; fy = 1919 - x; }
				if (((u32 *)(fake_fb + (size_t)fy * stride))[fx] != 0xff000000u)
					nonblack++;
			}
		CHECK("the Tux logo area contains colour", nonblack > 200);
	}
	CHECK("the marker words in the scanline tail were not touched by the blit", readl_relaxed(fake_fb + MARK_REQ_OFF) == 0 && readl_relaxed(fake_fb + MARK_ACK_OFF) == 0);
	CHECK("the title shows the cached kernel release", strcmp(release, "6.6.30-android15-8-gda510ce0fdcf-4k") == 0);
	/* a pixel row must not stray outside the 1200 visible pixels: the 16 pixel tail of every scanline stays untouched */
	{
		unsigned int bad = 0;
		for (unsigned int y = 0; y < height; y++)
			for (unsigned int x = width; x < stride / 4; x++)
				if (((u32 *)(fake_fb + (size_t)y * stride))[x] != 0)
					bad++;
		CHECK("the scanline tails (beyond 1200 px) were never written", bad == 0);
	}

	/* ---- a panic: written synchronously, even if the lock belongs to a dead CPU */
	{
		u8 *copy = malloc((size_t)stride * height);
		memcpy(copy, fake_fb, (size_t)stride * height);
		oops_in_progress = 1;
		trylock_result = 0;	/* both locks "taken" by somebody else */
		feed("Kernel panic - not syncing: boom, the end of the demo\n");
		oops_in_progress = 0;
		trylock_result = 1;
		CHECK("a panic line is rendered at once even with the locks held", memcmp(copy, fake_fb, (size_t)stride * height) != 0);
		CHECK("the panic line is red", rowcol[cur_row - 1] == C_ERR);
		free(copy);
	}

	/* ---- the work function renders when dirty and requeues itself */
	{
		int rq = requeued;
		feed("one more line\n");
		render_fn(NULL);
		CHECK("the work function requeues itself while running", requeued == rq + 1 && !dirty);
	}

	/* ---- the stop handshake */
	{
		unsigned int unreg = unregistered_calls;
		int rq = requeued;
		writel_relaxed(MARK_REQ, fake_fb + MARK_REQ_OFF);	/* what simpledrm does on its first plane update */
		render_fn(NULL);
		CHECK("the console acknowledges the request", readl_relaxed(fake_fb + MARK_ACK_OFF) == MARK_ACK);
		CHECK("the console unregistered itself, freed its buffers and does not requeue", stopped && unregistered_calls == unreg + 1 && cells == NULL && shadow == NULL && requeued == rq);
		feed("after stop\n");	/* must be a no-op (and must not crash with freed buffers) */
		render_fn(NULL);
		CHECK("writes and work after the stop are ignored", true);
	}
	printf("\n%d check(s) failed\n", fails);
	if (argc >= 3) {
		FILE *f = fopen(argv[2], "wb");
		fwrite(fake_fb, 1, (size_t)stride * height, f);
		fclose(f);
	}
	return fails ? 1 : 0;
}
