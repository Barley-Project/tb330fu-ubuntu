// SPDX-License-Identifier: GPL-2.0
/*
 * tb330fu_intpol - one-shot MediaTek MCUSYS INT_POL helper for the TB330FU (MT8786/MT6768) bring-up.
 *
 * The ROW 6.6 kernel declares every peripheral SPI as LEVEL_HIGH/EDGE_RISING and contains no mt6577-sysirq driver; the
 * inversion in front of the GIC is expected to be set up by someone else.  On the CN-LK/CN-GZ/ROW-kernel mix nobody does,
 * and several I2C interrupts storm (see the TB330FU notes).  This module dumps the polarity and GIC state, and with apply=1
 * programs INT_POL_CTL like the CN 4.19 kernel's sysirq driver would have for the CN device tree.
 */
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>

#define INT_POL_CTL0	0x0c53a650UL	/* 20 words, bit n of word w = SPI (32*w + n); 1 = invert */
#define NWORDS		20
#define GICD_BASE	0x0c000000UL

static int apply;
module_param(apply, int, 0444);
MODULE_PARM_DESC(apply, "1 = program INT_POL per the CN device-tree polarity map");

static int dump = 1;
module_param(dump, int, 0444);
MODULE_PARM_DESC(dump, "1 = log INT_POL / GIC distributor words");

/* SPIs that the CN device tree declares LEVEL_LOW or EDGE_FALLING and the ROW tree declares LEVEL_HIGH/EDGE_RISING: invert */
static const u16 inv_spi[] = {
	66, 73, 75, 78, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108,
	109, 110, 111, 112, 113, 114, 115, 116, 117, 133, 134, 135, 136, 138, 139, 141,
	142, 145, 146, 147, 148, 157, 158, 159, 160, 161, 163, 164, 165, 166, 169, 170,
	174, 176, 195, 203, 223, 224, 225, 226, 228, 229, 230, 231, 232, 233, 234, 235,
	236, 237, 238, 239, 240, 242, 243, 245, 246, 247, 250, 251, 252, 256, 257, 258,
	259, 261, 271, 273, 274, 275, 276, 277, 278, 283, 284, 285, 293, 295,
};

/* SPIs declared active-high in both trees: must NOT be inverted */
static const u16 keep_spi[] = {
	18, 19, 22, 23, 24, 25, 26, 27, 28, 29, 71, 177, 180, 186, 194, 205,
	207, 215, 218, 219, 220, 221, 222, 287, 296, 297, 298, 299, 300, 305,
};

static void log_words(const char *what, void __iomem *base, int first, int n)
{
	char line[200];
	int i, len = 0;

	for (i = 0; i < n; i++)
		len += scnprintf(line + len, sizeof(line) - len, " %08x", readl(base + 4 * (first + i)));
	pr_info("tb330fu_intpol: %s[%d..%d]=%s\n", what, first, first + n - 1, line);
}

static int __init tb330fu_intpol_init(void)
{
	void __iomem *pol, *gic;
	u32 oldv[NWORDS], newv[NWORDS];
	int i, changed = 0;

	pol = ioremap(INT_POL_CTL0, NWORDS * 4);
	gic = ioremap(GICD_BASE, 0x1000);
	if (!pol || !gic) {
		pr_err("tb330fu_intpol: ioremap failed pol=%p gic=%p\n", pol, gic);
		if (pol)
			iounmap(pol);
		if (gic)
			iounmap(gic);
		return -ENOMEM;
	}

	pr_info("tb330fu_intpol: start apply=%d dump=%d (INT_POL 0x%lx)\n", apply, dump, INT_POL_CTL0);
	if (dump) {
		log_words("INT_POL", pol, 0, NWORDS);
		/* INTID 128..255 = SPI 96..223 */
		log_words("GICD_IGROUPR", gic + 0x080, 4, 4);
		log_words("GICD_ISENABLER", gic + 0x100, 4, 4);
		log_words("GICD_ISPENDR", gic + 0x200, 4, 4);
		log_words("GICD_ISACTIVER", gic + 0x300, 4, 4);
		log_words("GICD_ICFGR", gic + 0xc00, 8, 8);
	}

	for (i = 0; i < NWORDS; i++)
		oldv[i] = newv[i] = readl(pol + 4 * i);

	if (apply) {
		size_t k;

		for (k = 0; k < ARRAY_SIZE(inv_spi); k++)
			newv[inv_spi[k] >> 5] |= BIT(inv_spi[k] & 31);
		for (k = 0; k < ARRAY_SIZE(keep_spi); k++)
			newv[keep_spi[k] >> 5] &= ~BIT(keep_spi[k] & 31);

		for (i = 0; i < NWORDS; i++) {
			if (newv[i] == oldv[i])
				continue;
			changed++;
			pr_info("tb330fu_intpol: word %d %08x -> %08x (flips %08x)\n", i, oldv[i], newv[i], oldv[i] ^ newv[i]);
			writel(newv[i], pol + 4 * i);
		}
		wmb();
		pr_info("tb330fu_intpol: %zu invert + %zu keep SPIs processed, %d words rewritten\n",
			ARRAY_SIZE(inv_spi), ARRAY_SIZE(keep_spi), changed);
		if (dump) {
			log_words("INT_POL(after)", pol, 0, NWORDS);
			for (i = 0; i < NWORDS; i++)
				if (readl(pol + 4 * i) != newv[i])
					pr_err("tb330fu_intpol: word %d did not take the new value (read back %08x, wanted %08x)\n",
					       i, readl(pol + 4 * i), newv[i]);
		}
	}

	iounmap(gic);
	iounmap(pol);
	return 0;
}

static void __exit tb330fu_intpol_exit(void)
{
}

module_init(tb330fu_intpol_init);
module_exit(tb330fu_intpol_exit);

MODULE_DESCRIPTION("TB330FU one-shot MediaTek INT_POL dump/program helper");
MODULE_LICENSE("GPL");
