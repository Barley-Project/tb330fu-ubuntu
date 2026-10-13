/* SPDX-License-Identifier: GPL-2.0 */
#ifndef TB330FU_GPU_H
#define TB330FU_GPU_H
#include <linux/types.h>

int tb330fu_gpu_power_init(void);
int tb330fu_gpu_power(bool on);
#endif
