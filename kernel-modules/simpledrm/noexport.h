/* SPDX-License-Identifier: GPL-2.0 */
/* force-included: the helper sources bundled into simpledrm.ko must not export (or re-export) anything */
#ifndef TB330FU_NOEXPORT_H
#define TB330FU_NOEXPORT_H
#include <linux/export.h>
#undef EXPORT_SYMBOL
#undef EXPORT_SYMBOL_GPL
#undef EXPORT_SYMBOL_NS
#undef EXPORT_SYMBOL_NS_GPL
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#define EXPORT_SYMBOL_NS(x, ns)
#define EXPORT_SYMBOL_NS_GPL(x, ns)
#endif
