#!/usr/bin/env python3
"""Exact-match patches of the 6.6.30 Panfrost + DRM scheduler sources for the TB330FU (every pattern must match exactly once).
usage: 520_patch_panfrost.py <dir with the copied sources>
"""
import sys

d = sys.argv[1]


def edit(fn, fnc):
    p = "%s/%s" % (d, fn)
    s = open(p, encoding="utf-8").read()
    open(p, "w", encoding="utf-8").write(fnc(s))


def once(s, old, new):
    assert s.count(old) == 1, ("pattern count %d" % s.count(old), old[:80])
    return s.replace(old, new)


def replace_func(s, sig, new_body):
    """replace the body of the function whose definition starts with sig (up to the first line that is exactly '}')"""
    i = s.index(sig)
    j = s.index("\n}\n", i) + 3
    return s[:i] + sig + "\n{\n" + new_body + "}\n" + s[j:]


def drv(s):
    s = once(s, "static const struct of_device_id dt_match[] = {\n",
             "static const struct panfrost_compatible tb330fu_data = {\n"
             "\t.num_supplies = 0,\n\t.supply_names = NULL,\n\t.num_pm_domains = 0,\n\t.pm_domain_names = NULL,\n};\n\n"
             "static const struct of_device_id dt_match[] = {\n"
             "\t{ .compatible = \"mediatek,mali\", .data = &tb330fu_data, },\n"
             "\t{ .compatible = \"arm,mali-valhall\", .data = &tb330fu_data, },\n")
    s = once(s, "module_platform_driver(panfrost_driver);\n",
             "int drm_sched_fence_slab_init(void);\n"
             "void drm_sched_fence_slab_fini(void);\n\n"
             "/* the Mali DT node has ged-supply = <&ged> and the vendor GED driver is not loaded: fw_devlink keeps the probe deferred until the deferred-probe\n"
             " * timeout (10 s after the last module load).  A stub driver that binds the /ged node satisfies the link at once. */\n"
             "static int tb330fu_ged_probe(struct platform_device *pdev)\n{\n\treturn 0;\n}\n\n"
             "static const struct of_device_id tb330fu_ged_match[] = {\n\t{ .compatible = \"mediatek,ged\" },\n\t{ }\n};\n\n"
             "static struct platform_driver tb330fu_ged_driver = {\n"
             "\t.probe = tb330fu_ged_probe,\n"
             "\t.driver = {\n\t\t.name = \"tb330fu-ged-stub\",\n\t\t.of_match_table = tb330fu_ged_match,\n\t},\n};\n\n"
             "static int __init tb330fu_pf_init(void)\n{\n"
             "\tint r = drm_sched_fence_slab_init();\n\n"
             "\tif (r)\n\t\treturn r;\n"
             "\tr = platform_driver_register(&tb330fu_ged_driver);\n"
             "\tif (r) {\n\t\tdrm_sched_fence_slab_fini();\n\t\treturn r;\n\t}\n"
             "\tr = platform_driver_register(&panfrost_driver);\n"
             "\tif (r) {\n\t\tplatform_driver_unregister(&tb330fu_ged_driver);\n\t\tdrm_sched_fence_slab_fini();\n\t}\n"
             "\treturn r;\n}\n\n"
             "static void __exit tb330fu_pf_exit(void)\n{\n"
             "\tplatform_driver_unregister(&panfrost_driver);\n"
             "\tplatform_driver_unregister(&tb330fu_ged_driver);\n"
             "\tdrm_sched_fence_slab_fini();\n}\n\n"
             "module_init(tb330fu_pf_init);\nmodule_exit(tb330fu_pf_exit);\n")
    return s


def dev(s):
    s = replace_func(s, "static int panfrost_clk_init(struct panfrost_device *pfdev)", "\treturn 0;\n")
    s = replace_func(s, "static void panfrost_clk_fini(struct panfrost_device *pfdev)", "")
    s = replace_func(s, "static int panfrost_reset_init(struct panfrost_device *pfdev)", "\treturn 0;\n")
    s = replace_func(s, "static void panfrost_reset_fini(struct panfrost_device *pfdev)", "")
    s = once(s, "int panfrost_device_init(struct panfrost_device *pfdev)\n{\n",
             "int panfrost_device_init(struct panfrost_device *pfdev)\n{\n"
             "\tint tb_err = tb330fu_gpu_power_init();\n\n"
             "\tif (!tb_err)\n\t\ttb_err = tb330fu_gpu_power(true);\n"
             "\tif (tb_err)\n\t\treturn tb_err;\n")
    i = s.index("void panfrost_device_fini(struct panfrost_device *pfdev)")
    j = s.index("\n}\n", i)
    s = s[:j] + "\n\ttb330fu_gpu_power(false);" + s[j:]
    s = once(s, '#include "panfrost_device.h"\n', '#include "panfrost_device.h"\n#include "tb330fu_gpu.h"\n')
    return s


def dvf(s):
    return once(s, "int panfrost_devfreq_init(struct panfrost_device *pfdev)\n{\n", "int panfrost_devfreq_init(struct panfrost_device *pfdev)\n{\n\treturn 0;\n")


def shr(s):
    s = replace_func(s, "void panfrost_gem_shrinker_init(struct drm_device *dev)", "")
    s = replace_func(s, "void panfrost_gem_shrinker_cleanup(struct drm_device *dev)", "")
    return s


def fence(s):
    s = once(s, "static int __init drm_sched_fence_slab_init(void)", "int drm_sched_fence_slab_init(void)")
    s = once(s, "static void __exit drm_sched_fence_slab_fini(void)", "void drm_sched_fence_slab_fini(void)")
    s = once(s, "module_init(drm_sched_fence_slab_init);\n", "")
    s = once(s, "module_exit(drm_sched_fence_slab_fini);\n", "")
    return s


edit("panfrost_drv.c", drv)
edit("panfrost_device.c", dev)
edit("panfrost_devfreq.c", dvf)
edit("panfrost_gem_shrinker.c", shr)
edit("sched_fence.c", fence)
edit("panfrost_job.c", lambda s: once(s, 'platform_get_irq_byname(to_platform_device(pfdev->dev), "job")', 'platform_get_irq_byname(to_platform_device(pfdev->dev), "JOB")'))
edit("panfrost_mmu.c", lambda s: once(s, 'platform_get_irq_byname(to_platform_device(pfdev->dev), "mmu")', 'platform_get_irq_byname(to_platform_device(pfdev->dev), "MMU")'))
edit("panfrost_gpu.c", lambda s: once(s, 'platform_get_irq_byname(to_platform_device(pfdev->dev), "gpu")', 'platform_get_irq_byname(to_platform_device(pfdev->dev), "GPU")'))
print("panfrost sources patched")
