#!/usr/bin/env python3
"""Generate the tb330fu_intpol kernel module source from work/intpol_map.json (derived from the CN device tree).

The module is a one-shot MediaTek MCUSYS interrupt-polarity helper:
  * logs INT_POL_CTL0..19 (0x0c53a650) and the GIC distributor state of the SPIs of interest
  * with apply=1 sets/clears the INT_POL bits so that every SPI that the CN device tree declares LEVEL_LOW/EDGE_FALLING is
    inverted in front of the GIC, and those declared LEVEL_HIGH/EDGE_RISING are not (the ROW 6.6 kernel declares everything
    LEVEL_HIGH/EDGE_RISING and has no sysirq driver; the CN 4.19 kernel's sysirq driver is what programmed these bits)

usage: 210_gen_intpol_module.py <intpol_map.json> <out_dir>
"""
import json
import os
import sys

m = json.load(open(sys.argv[1]))
out = sys.argv[2]
os.makedirs(out, exist_ok=True)
inv = sorted(m["invert"])
keep = sorted(m["keep"])


def arr(name, vals):
    lines = []
    row = []
    for v in vals:
        row.append("%d" % v)
        if len(row) == 16:
            lines.append("\t" + ", ".join(row) + ",")
            row = []
    if row:
        lines.append("\t" + ", ".join(row) + ",")
    return "static const u16 %s[] = {\n%s\n};\n" % (name, "\n".join(lines))


HEAD = r'''// SPDX-License-Identifier: GPL-2.0
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

'''

TAIL = r'''
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
'''

src = HEAD
src += "/* SPIs that the CN device tree declares LEVEL_LOW or EDGE_FALLING and the ROW tree declares LEVEL_HIGH/EDGE_RISING: invert */\n"
src += arr("inv_spi", inv)
src += "\n/* SPIs declared active-high in both trees: must NOT be inverted */\n"
src += arr("keep_spi", keep)
src += TAIL
open(os.path.join(out, "tb330fu_intpol.c"), "w", encoding="utf-8", newline="\n").write(src)
open(os.path.join(out, "Makefile"), "w", encoding="utf-8", newline="\n").write("obj-m += tb330fu_intpol.o\n")
print("generated %s : %d invert SPIs, %d keep SPIs" % (out, len(inv), len(keep)))
