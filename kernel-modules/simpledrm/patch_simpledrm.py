#!/usr/bin/env python3
"""Generate the TB330FU simpledrm module source (sd_main.c) from the kernel's drivers/gpu/drm/tiny/simpledrm.c.

usage: patch_simpledrm.py <kernel tree>/drivers/gpu/drm/tiny/simpledrm.c <tb330fu_flip.inc> <out sd_main.c>

Every replacement must match exactly once (the script aborts otherwise), so a different kernel version is detected instead of silently mis-patched.
What is added to the stock driver:
  * the aperture call is compiled out (the firmware framebuffer is handed over by the bootloader, there is nothing to evict)
  * panel orientation as a module parameter (default RIGHT_UP: the panel is a portrait panel built into a landscape tablet)
  * console handshake: tells the boot console (tb330fu_con) to stop drawing before the first blit
  * the scan-out ring (tb330fu_flip.inc): the commit is blitted into a buffer that is not scanned, then the OVL0 layer address is flipped
  * drm_name / gpu_sync parameters (Mesa picks Panfrost for rendering when the DRM driver reports the name "mediatek")
  * connector type DSI (mutter treats only built-in panels as touchscreen targets) and a 90 Hz mode (the panel's real refresh rate)
"""
import sys

src = open(sys.argv[1], encoding="utf-8").read()
flip = open(sys.argv[2], encoding="utf-8").read()
hdr = '''
/* TB330FU: not available in the GKI kernel; the firmware framebuffer is handed over by the bootloader (nothing to evict) */
#define devm_aperture_acquire_from_firmware(dev, base, size) 0
/* The panel is a native portrait 1200x1920 mounted in a landscape-first tablet; the camera side (the intended "top") is the panel's RIGHT edge,
 * i.e. DRM_MODE_PANEL_ORIENTATION_RIGHT_UP (3).  User space (KWin) reads the "panel orientation" connector property and rotates the desktop and the
 * touch coordinates.  Parameter: -1 = do not report an orientation, 0 normal, 1 upside down, 2 left side up, 3 right side up. */
static int tb330fu_orientation = DRM_MODE_PANEL_ORIENTATION_RIGHT_UP;
module_param_named(orientation, tb330fu_orientation, int, 0444);
MODULE_PARM_DESC(orientation, "panel orientation reported to user space: -1 none, 0 normal, 1 upside down, 2 left side up, 3 right side up (default)");
#define drm_connector_set_panel_orientation_with_quirk(c, o, w, h) drm_connector_set_panel_orientation((c), tb330fu_orientation)
'''
marker = "#include <drm/drm_probe_helper.h>\n"
assert src.count(marker) == 1
src = src.replace(marker, marker + hdr)
src = src.replace("#include <drm/drm_probe_helper.h>\n", "#include <drm/drm_probe_helper.h>\n#include <linux/delay.h>\n#include <linux/ktime.h>\n#include <linux/io.h>\n#include <linux/backlight.h>\n#include <linux/fb.h>\n", 1)
anchor = "static void simpledrm_primary_plane_helper_atomic_update(struct drm_plane *plane,\n"
assert src.count(anchor) == 1
helper = '''/* TB330FU: tell the boot console (tb330fu_con) to stop drawing before the first blit, and wait for its acknowledgement */
static void tb330fu_con_handshake(struct simpledrm_device *sdev)
{
	static bool done;
	int i;

	if (done)
		return;
	done = true;
	iosys_map_wr(&sdev->screen_base, 4 * 1210, u32, 0x54583330);
	wmb();
	for (i = 0; i < 60; i++) {
		if (iosys_map_rd(&sdev->screen_base, 4 * 1211, u32) == 0x54583331)
			break;
		msleep(10);
	}
}

'''
old_loop = '''drm_atomic_helper_damage_iter_init(&iter, old_plane_state, plane_state);
	drm_atomic_for_each_plane_damage(&iter, &damage) {
		struct drm_rect dst_clip = plane_state->dst;
		struct iosys_map dst = sdev->screen_base;

		if (!drm_rect_intersect(&dst_clip, &damage))
			continue;

		iosys_map_incr(&dst, drm_fb_clip_offset(sdev->pitch, sdev->format, &dst_clip));
		drm_fb_blit(&dst, &sdev->pitch, sdev->format->format, shadow_plane_state->data,
			    fb, &damage);
	}
'''
assert src.count(old_loop) == 1
new_loop = '''tb330fu_con_handshake(sdev);
	tb330fu_sync_fb_for_cpu(dev, fb);

	if (tb330fu_flip_active()) {
		tb330fu_flip_update(sdev, old_plane_state, plane_state, shadow_plane_state, fb);
	} else if (tb330fu_flip_ok && tb330fu_front) {
		/* flip=0 was written at run time while buffer 1 is scanned: full frame into buffer 0, then point the scan-out back at it */
		struct drm_rect full = { 0, 0, sdev->mode.hdisplay, sdev->mode.vdisplay };
		struct iosys_map d0 = sdev->screen_base;

		drm_fb_blit(&d0, &sdev->pitch, sdev->format->format, shadow_plane_state->data, fb, &full);
		tb330fu_flip_to(sdev, 0);
		tb330fu_synced = false;
	} else {
		tb330fu_synced = false;
		drm_atomic_helper_damage_iter_init(&iter, old_plane_state, plane_state);
		drm_atomic_for_each_plane_damage(&iter, &damage) {
			struct drm_rect dst_clip = plane_state->dst;
			struct iosys_map dst = sdev->screen_base;

			if (!drm_rect_intersect(&dst_clip, &damage))
				continue;

			iosys_map_incr(&dst, drm_fb_clip_offset(sdev->pitch, sdev->format, &dst_clip));
			drm_fb_blit(&dst, &sdev->pitch, sdev->format->format, shadow_plane_state->data,
				    fb, &damage);
		}
	}

	tb330fu_screen_set(true);
'''
src = src.replace(old_loop, new_loop)
src = src.replace(anchor, helper + flip + "\n" + anchor)
dis = "iosys_map_memset(&sdev->screen_base, 0, 0, sdev->pitch * sdev->mode.vdisplay);\n"
assert src.count(dis) == 1
src = src.replace(dis, dis + "\ttb330fu_flip_clear(sdev);\n\ttb330fu_screen_set(false);\n")
mapa = "iosys_map_set_vaddr_iomem(&sdev->screen_base, screen_base);\n"
assert src.count(mapa) == 1
src = src.replace(mapa, mapa + "\n\t\ttb330fu_flip_init(sdev, pdev, res->start);\n\t\ttb330fu_bl_init(sdev);\n")
rem = "\tdrm_dev_unplug(dev);\n}\n"
assert src.count(rem) == 1
src = src.replace(rem, "\ttb330fu_flip_restore(sdev);\n" + rem)

# ---- second stage: driver name, connector type and refresh rate ----
s = src
old = "\t.name\t\t\t= DRIVER_NAME,\n"
assert s.count(old) == 1, s.count(old)
s = s.replace(old, "\t.name\t\t\t= tb330fu_drm_name,\n")
# the panel is a built-in MIPI DSI panel.  With DRM_MODE_CONNECTOR_Unknown mutter does not see a "laptop panel" (monitor is-builtin=false), maps no
# touchscreen to it (no EDID, so no GSettings match is possible either) and the touch coordinates are not rotated with the display.
old = "\t\t\t\t DRM_MODE_CONNECTOR_Unknown);\n"
assert s.count(old) == 1, s.count(old)
s = s.replace(old, "\t\t\t\t DRM_MODE_CONNECTOR_DSI);\n")
# the mode reported to user space was a made-up 60 Hz (DRM_MODE_INIT(60, ...): a simple-framebuffer node carries no timing).  The panel is a BOE HX83102J whose
# vendor driver ("boe hx83102j vdo 90hz") has a single mode, 1200x1920 with a 260234 kHz pixel clock, htotal 1405, vtotal 2058 = exactly 90.00 Hz, and the bootloader
# leaves the display running in it: report 90 Hz so that the compositor paces its frames accordingly.
anchor = "#define drm_connector_set_panel_orientation_with_quirk(c, o, w, h) drm_connector_set_panel_orientation((c), tb330fu_orientation)\n"
assert s.count(anchor) == 1
s = s.replace(anchor, anchor + "static unsigned int tb330fu_refresh = 90;\nmodule_param_named(refresh, tb330fu_refresh, uint, 0444);\n"
              "MODULE_PARM_DESC(refresh, \"refresh rate in Hz reported to user space (the panel scans out at 90 Hz)\");\n")
old = "\t\tDRM_MODE_INIT(60, width, height, width_mm, height_mm)\n"
assert s.count(old) == 1, s.count(old)
s = s.replace(old, "\t\tDRM_MODE_INIT(tb330fu_refresh, width, height, width_mm, height_mm)\n")

open(sys.argv[3], "w", encoding="utf-8").write(s)
print("sd_main.c written (%d bytes)" % len(s))
