// SPDX-License-Identifier: GPL-2.0
/*
 * TB330FU: lets UNSIGNED modules export symbols that the GKI module protection reserves for signed modules.
 *
 * The gda debug kernel (CONFIG_MODULE_SIG_PROTECT) refuses a module without a valid signature that exports a "protected" symbol
 * ("rfkill: exports protected symbol rfkill_alloc", -EACCES).  The GKI modules of the ROW074 system_dlkm (rfkill, bluetooth, hci_uart, ...) and the vendor
 * cfg80211 are signed by another build key and therefore count as unsigned here.
 *
 * The check is gki_is_module_protected_export(name) (kernel/module/gki_module.c, a global function; its lookup table is read-only after init, writing it
 * panics).  A kretprobe on it forces the return value to false.  The probe stays registered while the module is loaded (keep it loaded while the Wi-Fi and
 * Bluetooth modules are inserted; it may be removed afterwards).
 */
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/module.h>

static int gku_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	regs->regs[0] = 0;	/* bool false: the symbol is not protected */
	return 0;
}

static struct kretprobe gku_krp = {
	.handler = gku_ret,
	.maxactive = 16,
	.kp.symbol_name = "gki_is_module_protected_export",
};

static int __init gku_init(void)
{
	int ret = register_kretprobe(&gku_krp);

	if (ret)
		pr_err("tb330fu_gkiunlock: register_kretprobe failed: %d\n", ret);
	else
		pr_info("tb330fu_gkiunlock: gki_is_module_protected_export() now always returns false (probe at %p)\n", gku_krp.kp.addr);
	return ret;
}

static void __exit gku_exit(void)
{
	unregister_kretprobe(&gku_krp);
}

module_init(gku_init);
module_exit(gku_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TB330FU: allow unsigned modules to export GKI protected symbols");
