// SPDX-License-Identifier: GPL-2.0
/*
 * The GKI "protected exports" list keeps unsigned modules from importing a few DRM helpers that Panfrost needs.  1:1 copies of the 6.6.30 helpers
 * (drivers/gpu/drm/drm_syncobj.c, drm_gem.c, drm_gem_shmem_helper.c), compiled into the module under the same names.
 * drm_gem_shmem_purge is a no-op: the Panfrost GEM shrinker is not registered in this build, so nothing is ever purged.
 */
#include <linux/dma-resv.h>
#include <linux/idr.h>
#include <linux/iosys-map.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_syncobj.h>
#include <drm/drm_utils.h>

/* drm_gem_vmap / drm_gem_vunmap (drm_gem.c) are protected exports as well: same logic through the object's vtable */
static int tb_gem_vmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	int ret;

	if (!obj->funcs->vmap)
		return -EOPNOTSUPP;

	ret = obj->funcs->vmap(obj, map);
	if (ret)
		return ret;
	else if (iosys_map_is_null(map))
		return -ENOMEM;

	return 0;
}

static void tb_gem_vunmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	if (iosys_map_is_null(map))
		return;

	if (obj->funcs->vunmap)
		obj->funcs->vunmap(obj, map);

	/* Always set the mapping to NULL. Callers may rely on this. */
	iosys_map_clear(map);
}

signed long drm_timeout_abs_to_jiffies(int64_t timeout_nsec)
{
	ktime_t abs_timeout, now;
	u64 timeout_ns, timeout_jiffies64;

	/* make 0 timeout means poll - absolute 0 doesn't seem valid */
	if (timeout_nsec == 0)
		return 0;

	abs_timeout = ns_to_ktime(timeout_nsec);
	now = ktime_get();

	if (!ktime_after(abs_timeout, now))
		return 0;

	timeout_ns = ktime_to_ns(ktime_sub(abs_timeout, now));

	timeout_jiffies64 = timeout_ns / TICK_NSEC;
	/*  clamp timeout to avoid infinite timeout */
	if (timeout_jiffies64 >= MAX_SCHEDULE_TIMEOUT - 1)
		return MAX_SCHEDULE_TIMEOUT - 1;

	return timeout_jiffies64 + 1;
}

static int objects_lookup(struct drm_file *filp, u32 *handle, int count, struct drm_gem_object **objs)
{
	int i, ret = 0;
	struct drm_gem_object *obj;

	spin_lock(&filp->table_lock);

	for (i = 0; i < count; i++) {
		/* Check if we currently have a reference on the object */
		obj = idr_find(&filp->object_idr, handle[i]);
		if (!obj) {
			ret = -ENOENT;
			break;
		}
		drm_gem_object_get(obj);
		objs[i] = obj;
	}
	spin_unlock(&filp->table_lock);

	return ret;
}

int drm_gem_objects_lookup(struct drm_file *filp, void __user *bo_handles, int count, struct drm_gem_object ***objs_out)
{
	int ret;
	u32 *handles;
	struct drm_gem_object **objs;

	if (!count)
		return 0;

	objs = kvmalloc_array(count, sizeof(struct drm_gem_object *), GFP_KERNEL | __GFP_ZERO);
	if (!objs)
		return -ENOMEM;

	*objs_out = objs;

	handles = kvmalloc_array(count, sizeof(u32), GFP_KERNEL);
	if (!handles) {
		ret = -ENOMEM;
		goto out;
	}

	if (copy_from_user(handles, bo_handles, count * sizeof(u32))) {
		ret = -EFAULT;
		goto out;
	}

	ret = objects_lookup(filp, handles, count, objs);
out:
	kvfree(handles);
	return ret;
}

int drm_gem_vmap_unlocked(struct drm_gem_object *obj, struct iosys_map *map)
{
	int ret;

	dma_resv_lock(obj->resv, NULL);
	ret = tb_gem_vmap(obj, map);
	dma_resv_unlock(obj->resv);

	return ret;
}

void drm_gem_vunmap_unlocked(struct drm_gem_object *obj, struct iosys_map *map)
{
	dma_resv_lock(obj->resv, NULL);
	tb_gem_vunmap(obj, map);
	dma_resv_unlock(obj->resv);
}

int drm_gem_shmem_madvise(struct drm_gem_shmem_object *shmem, int madv)
{
	dma_resv_assert_held(shmem->base.resv);

	if (shmem->madv >= 0)
		shmem->madv = madv;

	madv = shmem->madv;

	return (madv >= 0);
}

void drm_gem_shmem_purge(struct drm_gem_shmem_object *shmem)
{
	/* never reached: the shrinker is not registered */
}
