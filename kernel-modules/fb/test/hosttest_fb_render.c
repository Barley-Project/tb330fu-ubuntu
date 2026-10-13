/* Host-side render of tb330fu_fb's paint_one(): shims for the kernel API, writes buffer 0/1/2 (stride 1216 px) to raw files. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint8_t u8;
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
#define writel_relaxed(v, p) (*(volatile u32 *)(p) = (v))
#define wmb()
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; };
struct resource { unsigned long start, end; };
struct platform_device { int x; };
struct simplefb_platform_data { unsigned int width, height, stride; const char *format; };
#define DEFINE_RES_MEM(a, b) { (a), (a) + (b) - 1 }
#define HZ 250
static unsigned long jiffies;
#define jiffies_to_msecs(x) ((x) * 4)
#define INIT_DELAYED_WORK(w, f) ((void)0)
#define schedule_delayed_work(w, d) ((void)0)
#define cancel_delayed_work_sync(w) ((void)0)
#define ioremap_wc(a, b) ((void *)0)
#define iounmap(p) ((void)0)
#define IS_ERR(p) 0
#define PTR_ERR(p) 0
#define platform_device_unregister(p) ((void)0)
#define PLATFORM_DEVID_AUTO (-2)
#define platform_device_register_resndata(a, b, c, d, e, f, g) ((struct platform_device *)0)
#include "../tb330fu_fb.c"
int main(void)
{
	unsigned int w = 1200, h = 1920, sp = 1216;
	for (unsigned idx = 0; idx < 3; idx++) {
		u32 *b = calloc((size_t)sp * h, 4);
		paint_one(b, sp, w, h, idx);
		char nm[64];
		snprintf(nm, sizeof nm, "/tmp/fbrender_%u.raw", idx);
		FILE *f = fopen(nm, "wb");
		fwrite(b, 4, (size_t)sp * h, f);
		fclose(f);
		free(b);
	}
	return 0;
}
