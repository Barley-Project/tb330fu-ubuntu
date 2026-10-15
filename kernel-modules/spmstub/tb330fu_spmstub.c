// SPDX-License-Identifier: GPL-2.0
/*
 * tb330fu_spmstub - stands in for the three functions of the vendor system power manager (MTK_INTERNAL_SPM.ko) that the audio driver imports.
 *
 * snd-soc-mt6768-afe.ko (the MT6768 audio front end) imports spm_resource_req(), mtk_idle_notifier_register() and RegisterConditionEnterSuspend()
 * from MTK_INTERNAL_SPM.ko.  Android uses them to tell the SPM that the audio hardware needs the 26 MHz clock / PLLs while it plays, and to hear about
 * the AP going into the SPM's deep idle states.  This port runs without cpuidle (cpuidle.off=1) and without the SPM driver, which drags in 17 more vendor
 * modules (SSPM, power gating, the GPU DVFS stack, thermal monitor ...) and starts the SPM firmware: nothing ever enters a deep idle state that would stop the
 * audio clocks, so the answers are "done" / "registered".
 *
 * The exact prototypes of the three vendor functions are not known (the module is stripped), so every stub takes no arguments that it looks at and returns 0.
 * The CRC that the audio module expects for each symbol is copied into the built stub afterwards (scripts/patch-crc.py), so the module loader accepts it.
 */
#include <linux/module.h>

int spm_resource_req(unsigned int user, unsigned int req_mask)
{
	return 0;
}
EXPORT_SYMBOL(spm_resource_req);

int mtk_idle_notifier_register(void *notifier)
{
	return 0;
}
EXPORT_SYMBOL(mtk_idle_notifier_register);

int RegisterConditionEnterSuspend(void *cb)
{
	return 0;
}
EXPORT_SYMBOL(RegisterConditionEnterSuspend);

static int __init spmstub_init(void)
{
	pr_info("tb330fu_spmstub: spm_resource_req / mtk_idle_notifier_register / RegisterConditionEnterSuspend are no-ops\n");
	return 0;
}

static void __exit spmstub_exit(void)
{
}

module_init(spmstub_init);
module_exit(spmstub_exit);
MODULE_DESCRIPTION("no-op stand-ins for the MTK_INTERNAL_SPM exports used by the audio driver");
MODULE_LICENSE("GPL");
