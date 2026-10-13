// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Small DRM helpers that the GKI kernel of the TB330FU does not export (GKI trims every export that no vendor module uses),
 * re-implemented 1:1 from the 6.6.30 sources (drm_fourcc.c, drm_plane_helper.c, drm_crtc_helper.c, drm_probe_helper.c) so that the
 * simpledrm bundle links without them.  Bodies are copies of the upstream functions; only the EXPORT_SYMBOLs are dropped.
 */
#include <linux/export.h>
#include <linux/module.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_modes.h>
#include <drm/drm_plane.h>
#include <linux/dma-resv.h>
#include <linux/iosys-map.h>

#include <drm/drm_device.h>
#include <drm/drm_gem.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

unsigned int drm_format_info_bpp(const struct drm_format_info *info, int plane)
{
	if (!info || plane < 0 || plane >= info->num_planes)
		return 0;

	return info->char_per_block[plane] * 8 /
	       (info->block_w[plane] * info->block_h[plane]);
}

int drm_plane_helper_atomic_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc *new_crtc = new_plane_state->crtc;
	struct drm_crtc_state *new_crtc_state = NULL;
	int ret;

	if (new_crtc)
		new_crtc_state = drm_atomic_get_new_crtc_state(state, new_crtc);

	ret = drm_atomic_helper_check_plane_state(new_plane_state, new_crtc_state,
						  DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING, false, false);
	if (ret)
		return ret;

	return 0;
}

int drm_crtc_helper_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct drm_crtc_state *new_crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	if (!new_crtc_state->enable)
		return 0;

	return drm_atomic_helper_check_crtc_primary_plane(new_crtc_state);
}

enum drm_mode_status drm_crtc_helper_mode_valid_fixed(struct drm_crtc *crtc, const struct drm_display_mode *mode,
						      const struct drm_display_mode *fixed_mode)
{
	if (mode->hdisplay != fixed_mode->hdisplay && mode->vdisplay != fixed_mode->vdisplay)
		return MODE_ONE_SIZE;
	else if (mode->hdisplay != fixed_mode->hdisplay)
		return MODE_ONE_WIDTH;
	else if (mode->vdisplay != fixed_mode->vdisplay)
		return MODE_ONE_HEIGHT;

	return MODE_OK;
}

int drm_connector_helper_get_modes_fixed(struct drm_connector *connector, const struct drm_display_mode *fixed_mode)
{
	struct drm_device *dev = connector->dev;
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(dev, fixed_mode);
	if (!mode) {
		drm_err(dev, "Failed to duplicate mode " DRM_MODE_FMT "\n", DRM_MODE_ARG(fixed_mode));
		return 0;
	}

	if (mode->name[0] == '\0')
		drm_mode_set_name(mode);

	mode->type |= DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	if (mode->width_mm)
		connector->display_info.width_mm = mode->width_mm;
	if (mode->height_mm)
		connector->display_info.height_mm = mode->height_mm;

	return 1;
}

/* drm_plane.c: drm_plane_check_pixel_format() + drm_any_plane_has_format() */
static int tb330fu_plane_check_pixel_format(struct drm_plane *plane, u32 format, u64 modifier)
{
	unsigned int i;

	for (i = 0; i < plane->format_count; i++) {
		if (format == plane->format_types[i])
			break;
	}
	if (i == plane->format_count)
		return -EINVAL;

	if (plane->funcs->format_mod_supported) {
		if (!plane->funcs->format_mod_supported(plane, format, modifier))
			return -EINVAL;
	} else {
		if (!plane->modifier_count)
			return 0;

		for (i = 0; i < plane->modifier_count; i++) {
			if (modifier == plane->modifiers[i])
				break;
		}
		if (i == plane->modifier_count)
			return -EINVAL;
	}

	return 0;
}

bool drm_any_plane_has_format(struct drm_device *dev, u32 format, u64 modifier)
{
	struct drm_plane *plane;

	drm_for_each_plane(plane, dev) {
		if (tb330fu_plane_check_pixel_format(plane, format, modifier) == 0)
			return true;
	}

	return false;
}

/* drm_gem.c: drm_gem_vmap() / drm_gem_vunmap() and their *_unlocked wrappers */
static int tb330fu_gem_vmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	int ret;

	dma_resv_assert_held(obj->resv);

	if (!obj->funcs->vmap)
		return -EOPNOTSUPP;

	ret = obj->funcs->vmap(obj, map);
	if (ret)
		return ret;
	else if (iosys_map_is_null(map))
		return -ENOMEM;

	return 0;
}

static void tb330fu_gem_vunmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	dma_resv_assert_held(obj->resv);

	if (iosys_map_is_null(map))
		return;

	if (obj->funcs->vunmap)
		obj->funcs->vunmap(obj, map);

	/* Always set the mapping to NULL. Callers may rely on this. */
	iosys_map_clear(map);
}

int drm_gem_vmap_unlocked(struct drm_gem_object *obj, struct iosys_map *map)
{
	int ret;

	dma_resv_lock(obj->resv, NULL);
	ret = tb330fu_gem_vmap(obj, map);
	dma_resv_unlock(obj->resv);

	return ret;
}

void drm_gem_vunmap_unlocked(struct drm_gem_object *obj, struct iosys_map *map)
{
	dma_resv_lock(obj->resv, NULL);
	tb330fu_gem_vunmap(obj, map);
	dma_resv_unlock(obj->resv);
}

/* drm_atomic_helper.c: drm_atomic_helper_check_crtc_primary_plane() */
int drm_atomic_helper_check_crtc_primary_plane(struct drm_crtc_state *crtc_state)
{
	struct drm_crtc *crtc = crtc_state->crtc;
	struct drm_device *dev = crtc->dev;
	struct drm_plane *plane;

	/* needs at least one primary plane to be enabled */
	drm_for_each_plane_mask(plane, dev, crtc_state->plane_mask) {
		if (plane->type == DRM_PLANE_TYPE_PRIMARY)
			return 0;
	}

	drm_dbg_atomic(dev, "[CRTC:%d:%s] primary plane missing\n", crtc->base.id, crtc->name);

	return -EINVAL;
}
