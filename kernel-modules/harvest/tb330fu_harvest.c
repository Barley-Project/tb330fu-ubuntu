// SPDX-License-Identifier: GPL-2.0
/*
 * tb330fu_harvest - diagnostic timer for the TB330FU Ubuntu bring-up.
 *
 * While Ubuntu runs on the tablet nothing is visible (no display driver exists yet, USB has no role switch) and MediaTek's
 * expdb only receives the console log when the kernel panics.  This module logs a heartbeat and, panic_after seconds after it
 * was loaded, calls panic() ON PURPOSE so that mrdump/aee saves the complete console log of the boot (stage1 + Ubuntu).
 * It lives in the kernel, so it keeps running after stage1 has done switch_root.  panic_after=0 only logs the heartbeat.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>

static unsigned int panic_after = 180;
module_param(panic_after, uint, 0444);
MODULE_PARM_DESC(panic_after, "seconds after load until the planned panic (0 = never)");

static unsigned int beat = 10;
module_param(beat, uint, 0444);
MODULE_PARM_DESC(beat, "heartbeat period in seconds");

static struct delayed_work harvest_work;
static unsigned long t0;

static void harvest_fn(struct work_struct *w)
{
	unsigned int el = jiffies_to_msecs(jiffies - t0) / 1000;

	pr_info("tb330fu_harvest: alive t+%us (planned panic after %us)\n", el, panic_after);
	if (panic_after && el >= panic_after)
		panic("tb330fu_harvest: planned panic at t+%us so that expdb keeps this boot's log", el);
	schedule_delayed_work(&harvest_work, beat * HZ);
}

static int __init harvest_init(void)
{
	if (!beat)
		beat = 10;
	t0 = jiffies;
	pr_info("tb330fu_harvest: start panic_after=%u beat=%u\n", panic_after, beat);
	INIT_DELAYED_WORK(&harvest_work, harvest_fn);
	schedule_delayed_work(&harvest_work, beat * HZ);
	return 0;
}

static void __exit harvest_exit(void)
{
	cancel_delayed_work_sync(&harvest_work);
}

module_init(harvest_init);
module_exit(harvest_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TB330FU harvest timer: heartbeat plus a planned panic so that expdb receives the boot log");
