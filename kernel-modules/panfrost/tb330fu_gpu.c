// SPDX-License-Identifier: GPL-2.0
/*
 * TB330FU (MT6768, Mali-G52 MC2 at 0x13040000): GPU power sequencing for Panfrost.
 *
 * The Android BSP powers the GPU through the vendor gpufreq driver (mtk_gpufreq_mt6768.ko: __gpufreq_power_control, __gpufreq_mtcmos_control,
 * __gpufreq_clock_control).  This file replays exactly those steps with the kernel clk / regulator APIs, using the handles that the "gpufreq" device
 * tree node already lists (clocks "subsys_mfg_cg", "mtcmos_mfg_async", "mtcmos_mfg", "mtcmos_mfg_core0", "mtcmos_mfg_core1") and the mfgcfg block
 * at 0x13000000:
 *   ON : regulator vgpu, regulator vsram_gpu, MTCMOS mfg_async -> mfg -> mfg_core0 -> mfg_core1, BG3D clock gate (subsys_mfg_cg),
 *        MFGCFG[0xb0] &= ~0x500, MFGCFG[0x20] |= 0x3c00000, MFGCFG[0x24] |= 1, MFGCFG[0x130] = 3
 *   OFF: MFGCFG[0xb4] = 1, MFGCFG[0x170] = 3, wait MFGCFG[0x178] bit 2, BG3D gate off, MTCMOS core1 -> core0 -> mfg -> mfg_async off, vsram_gpu, vgpu off
 * The PLL (mfgpll, 535 MHz as left by the preloader) and the clock mux are not touched; voltages stay at the values the PMIC drivers report.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

#include "tb330fu_gpu.h"

static int vgpu_uv;
module_param(vgpu_uv, int, 0444);
MODULE_PARM_DESC(vgpu_uv, "if non-zero: set vgpu to this voltage (uV) before enabling");
static int vsram_uv;
module_param(vsram_uv, int, 0444);
MODULE_PARM_DESC(vsram_uv, "if non-zero: set vsram_gpu to this voltage (uV) before enabling");

static const char *const pg_names[4] = { "mtcmos_mfg_async", "mtcmos_mfg", "mtcmos_mfg_core0", "mtcmos_mfg_core1" };

static struct regulator *r_vgpu, *r_vsram;
static struct clk *c_pg[4], *c_cg;
static void __iomem *mfgcfg;
static bool inited, powered;
static DEFINE_MUTEX(pwr_lock);

static void rmw(unsigned int off, u32 clr, u32 set)
{
	u32 v = readl(mfgcfg + off);

	writel((v & ~clr) | set, mfgcfg + off);
}

int tb330fu_gpu_power_init(void)
{
	struct device_node *np, *cfg;
	struct device *gdev = NULL;
	int i, ret = 0;

	mutex_lock(&pwr_lock);
	if (inited)
		goto out;
	np = of_find_compatible_node(NULL, NULL, "mediatek,gpufreq");
	cfg = of_find_compatible_node(NULL, NULL, "mediatek,mfgcfg");
	if (!np || !cfg) {
		pr_err("tb330fu_gpu: gpufreq (%p) or mfgcfg (%p) node missing\n", np, cfg);
		ret = -ENODEV;
		goto out;
	}
	mfgcfg = of_iomap(cfg, 0);
	if (!mfgcfg) {
		ret = -ENOMEM;
		goto out;
	}
	c_cg = of_clk_get_by_name(np, "subsys_mfg_cg");
	if (IS_ERR(c_cg)) {
		ret = PTR_ERR(c_cg);
		pr_err("tb330fu_gpu: clock subsys_mfg_cg: %d\n", ret);
		goto out;
	}
	for (i = 0; i < 4; i++) {
		c_pg[i] = of_clk_get_by_name(np, pg_names[i]);
		if (IS_ERR(c_pg[i])) {
			ret = PTR_ERR(c_pg[i]);
			pr_err("tb330fu_gpu: clock %s: %d\n", pg_names[i], ret);
			goto out;
		}
	}
	/* the vendor driver asks for the regulators with the "gpufreq" platform device as consumer */
	gdev = bus_find_device_by_name(&platform_bus_type, NULL, "gpufreq");
	if (!gdev) {
		pr_err("tb330fu_gpu: platform device gpufreq not found\n");
		ret = -ENODEV;
		goto out;
	}
	r_vgpu = regulator_get(gdev, "vgpu");
	if (IS_ERR(r_vgpu)) {
		ret = PTR_ERR(r_vgpu);
		pr_err("tb330fu_gpu: regulator vgpu: %d\n", ret);
		goto out;
	}
	r_vsram = regulator_get(gdev, "vsram_gpu");
	if (IS_ERR(r_vsram)) {
		ret = PTR_ERR(r_vsram);
		pr_err("tb330fu_gpu: regulator vsram_gpu: %d\n", ret);
		goto out;
	}
	inited = true;
	pr_info("tb330fu_gpu: handles ok (vgpu %d uV, vsram_gpu %d uV, mfgcfg %p)\n", regulator_get_voltage(r_vgpu), regulator_get_voltage(r_vsram), mfgcfg);
out:
	if (gdev)
		put_device(gdev);
	mutex_unlock(&pwr_lock);
	return ret;
}

static void undo_on(int step)
{
	int i;

	if (step >= 5)
		clk_disable_unprepare(c_cg);
	for (i = (step >= 4 ? 3 : step - 2); i >= 0; i--)
		clk_disable_unprepare(c_pg[i]);
	if (step >= 2)
		regulator_disable(r_vsram);
	regulator_disable(r_vgpu);
}

int tb330fu_gpu_power(bool on)
{
	int i, ret = 0, tries;

	mutex_lock(&pwr_lock);
	if (!inited) {
		ret = -ENODEV;
		goto out;
	}
	if (on == powered)
		goto out;
	if (on) {
		if (vgpu_uv)
			regulator_set_voltage(r_vgpu, vgpu_uv, vgpu_uv + 12500);
		if (vsram_uv)
			regulator_set_voltage(r_vsram, vsram_uv, vsram_uv + 12500);
		ret = regulator_enable(r_vgpu);
		if (ret) {
			pr_err("tb330fu_gpu: enable vgpu: %d\n", ret);
			goto out;
		}
		ret = regulator_enable(r_vsram);
		if (ret) {
			pr_err("tb330fu_gpu: enable vsram_gpu: %d\n", ret);
			undo_on(1);
			goto out;
		}
		for (i = 0; i < 4; i++) {
			ret = clk_prepare_enable(c_pg[i]);
			if (ret) {
				pr_err("tb330fu_gpu: enable %s: %d\n", pg_names[i], ret);
				undo_on(2 + i);
				goto out;
			}
		}
		ret = clk_prepare_enable(c_cg);
		if (ret) {
			pr_err("tb330fu_gpu: enable subsys_mfg_cg: %d\n", ret);
			undo_on(4);
			goto out;
		}
		rmw(0xb0, 0x500, 0);
		rmw(0x20, 0, 0x3c00000);
		rmw(0x24, 0, 0x1);
		writel(3, mfgcfg + 0x130);
		powered = true;
		pr_info("tb330fu_gpu: powered on (vgpu %d uV, vsram_gpu %d uV)\n", regulator_get_voltage(r_vgpu), regulator_get_voltage(r_vsram));
	} else {
		writel(1, mfgcfg + 0xb4);
		writel(3, mfgcfg + 0x170);
		for (tries = 0; tries < 1000 && !(readl(mfgcfg + 0x178) & 4); tries++)
			udelay(10);
		if (tries >= 1000)
			pr_warn("tb330fu_gpu: MFGCFG+0x178 bit 2 never set, powering off anyway\n");
		clk_disable_unprepare(c_cg);
		for (i = 3; i >= 0; i--)
			clk_disable_unprepare(c_pg[i]);
		regulator_disable(r_vsram);
		regulator_disable(r_vgpu);
		powered = false;
		pr_info("tb330fu_gpu: powered off\n");
	}
out:
	mutex_unlock(&pwr_lock);
	return ret;
}
