// SPDX-License-Identifier: GPL-2.0
/*
 * tb330fu_ppmtemp - answers the CPU temperature callbacks of the vendor power policy manager (mtk_ppm_v3).
 *
 * With the vendor CPU DVFS stack loaded (CPU_DVFS.ko needs mtk_ppm_v3.ko) the PPM asks for the temperature of the two CPU clusters whenever it recomputes its
 * limits - about 20 times per second - and prints "ppm_get_cpu_temp, s_ppm_thermal_cpuL_temp_cb not init!" every time, because the callbacks are registered by
 * the vendor thermal driver (thermal_monitor.ko). That module drags in the vendor GPU DVFS modules, which this port does not use (Panfrost powers the GPU).
 *
 * This module registers callbacks of its own. There is no CPU temperature sensor driver, so the value is whatever user space writes into
 *     /sys/module/tb330fu_ppmtemp/parameters/cpu_temp_mc      (milli-degrees Celsius)
 * (tb330fu-thermalguard keeps it at the battery temperature plus an offset); the default is 45 C. The PPM only uses the value for its power model.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>

/* exported by mtk_ppm_v3.ko: registers the two "cluster temperature in milli-degrees" callbacks (L = little/LL cluster, B = big cluster) */
extern void mt_ppm_thermal_get_cpu_cluster_temp_cb(int (*cb_l)(void), int (*cb_b)(void));

static int cpu_temp_mc = 45000;
module_param(cpu_temp_mc, int, 0644);
MODULE_PARM_DESC(cpu_temp_mc, "CPU temperature in milli-degrees Celsius reported to the vendor PPM");

static int tb330fu_temp_l(void)
{
	return READ_ONCE(cpu_temp_mc);
}

static int tb330fu_temp_b(void)
{
	return READ_ONCE(cpu_temp_mc);
}

static int __init ppmtemp_init(void)
{
	mt_ppm_thermal_get_cpu_cluster_temp_cb(tb330fu_temp_l, tb330fu_temp_b);
	pr_info("tb330fu_ppmtemp: PPM CPU temperature callbacks registered (cpu_temp_mc = %d)\n", cpu_temp_mc);
	return 0;
}

static void __exit ppmtemp_exit(void)
{
	mt_ppm_thermal_get_cpu_cluster_temp_cb(NULL, NULL);
}

module_init(ppmtemp_init);
module_exit(ppmtemp_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TB330FU: CPU temperature callbacks for the vendor PPM");
