/*
 * TB330FU: tear-free scan-out with a ring of two or three buffers.  LK's framebuffer region holds several equally sized buffers (0x8E8000 apart,
 * 4864 x 1920 bytes).  The display engine (OVL0 layer 0) reads ONE buffer continuously.  Blitting a frame into that very buffer shows the update as a sweep (the
 * panel scans portrait-native, so on a landscape desktop it looks like a CRT refreshing column by column).  With flip=1 every commit is blitted into a buffer that
 * is NOT scanned and then the OVL layer's address register is pointed at it (LK's end-address formula, the layer is read flipped).
 *
 * The register is latched at the start of a refresh, so the buffer that was scanned stays in use for up to one refresh period after the flip.  With two buffers a
 * commit therefore has to wait (hold_ms) before it may overwrite the previous front buffer: at most one commit per refresh period plus the blit time (about 45 fps
 * on a 90 Hz panel).  With three buffers the buffer that is written next is the one that was flipped away from before the previous flip, which is free long ago:
 * no waiting in the steady state (bufs=3, the default).
 *
 * Every buffer lags behind the newest frame, so each one keeps the union of the damage of all frames it has not seen yet (dirty[]).  Everything is checked at
 * probe: the OVL register must currently point at buffer 0, otherwise the old single-buffer path is used.
 */
#define TB_MAXBUF 3

static int tb330fu_flip = 1;
module_param_named(flip, tb330fu_flip, int, 0644);
MODULE_PARM_DESC(flip, "1 = scan-out ring (flip the OVL layer address between buffers), 0 = blit into the scanned buffer");
static int tb330fu_nbuf = 3;
module_param_named(bufs, tb330fu_nbuf, int, 0444);
MODULE_PARM_DESC(bufs, "number of scan-out buffers in the ring (2 or 3)");
static unsigned long tb330fu_buf_pa[TB_MAXBUF] = { 0, 0x7d1c8000UL, 0x7dab0000UL };
module_param_named(buf1, tb330fu_buf_pa[1], ulong, 0444);
MODULE_PARM_DESC(buf1, "physical address of the second scan-out buffer");
module_param_named(buf2, tb330fu_buf_pa[2], ulong, 0444);
MODULE_PARM_DESC(buf2, "physical address of the third scan-out buffer");
static unsigned long tb330fu_ovl_addr_pa = 0x1400b000UL + 0xf40UL;
module_param_named(ovl_addr_reg, tb330fu_ovl_addr_pa, ulong, 0444);
MODULE_PARM_DESC(ovl_addr_reg, "physical address of the OVL0 layer 0 address register");
static unsigned int tb330fu_hold_ms = 13;
module_param_named(hold_ms, tb330fu_hold_ms, uint, 0644);
MODULE_PARM_DESC(hold_ms, "minimum time between flipping away from a buffer and writing into it again (one refresh period plus margin)");


/* GPU rendering through Mesa's kmsro.  Mesa chooses the render driver from the DRM driver NAME: reporting "mediatek" (a kmsro-capable KMS driver) makes it
 * render with the Panfrost render node into dumb buffers of this device, which this driver then blits to the scan-out buffer.  The name is a run-time parameter
 * (read by user space when it opens the device), so the GPU service switches it only after Panfrost came up.  The GPU writes the buffer behind the CPU's back
 * (non-coherent DMA), so with gpu_sync=1 the CPU cache lines of the source buffer are invalidated before every blit. */
static char tb330fu_drm_name[32] = DRIVER_NAME;
module_param_string(drm_name, tb330fu_drm_name, sizeof(tb330fu_drm_name), 0644);
MODULE_PARM_DESC(drm_name, "name the DRM driver reports to user space (\"mediatek\" = Mesa uses the Panfrost render node, \"simpledrm\" = software rendering)");
static int tb330fu_gpu_sync;
module_param_named(gpu_sync, tb330fu_gpu_sync, int, 0644);
MODULE_PARM_DESC(gpu_sync, "1 = invalidate the CPU cache of the source buffer before every blit (a GPU rendered into it); never use with software rendering");

static void tb330fu_sync_fb_for_cpu(struct drm_device *dev, struct drm_framebuffer *fb)
{
	struct drm_gem_object *obj;
	struct sg_table *sgt;

	if (!tb330fu_gpu_sync)
		return;
	obj = drm_gem_fb_get_obj(fb, 0);
	if (!obj || obj->import_attach)
		return;
	sgt = drm_gem_shmem_get_pages_sgt(to_drm_gem_shmem_obj(obj));
	if (IS_ERR(sgt))
		return;
	dma_sync_sgtable_for_cpu(dev->dev, sgt, DMA_FROM_DEVICE);
}

/*
 * Backlight and screen on/off.
 *
 * The panel backlight is driven by the display PWM block DISP_PWM0 (0x1100e000). The bootloader programs it (period 1023, duty 494 = LK's "level 494/1024")
 * and leaves it running; nothing else controls it, so this driver registers /sys/class/backlight/tb330fu_bl on it (GNOME's brightness slider uses that):
 *
 *   0x00  DISP_PWM_EN       bit 0 = PWM running (set by LK, checked here)
 *   0x1c  DISP_PWM_CON_1    bits 28:16 = high width (duty), bits 11:0 = period
 *   0x2c  read only         the value the PWM generates right now (follows 0x1c)
 *   0x80  BLS_DEBUG         bits 1:0 = 3: disable the double buffering so that a write to 0x1c takes effect immediately
 *   0x08  (reset) - NEVER touch it: writing 1 then 0 resets the whole block, the PWM stops with duty 0 and the screen goes black
 *
 * Screen on/off follows the display pipeline: when the CRTC/plane is disabled (DPMS off, idle blank, power key) bl_power becomes FB_BLANK_POWERDOWN and the
 * backlight goes off; the first frame after the pipeline was enabled again switches it back on. (It is a class device inside this module on purpose: a
 * separate backlight module cannot be used from here, the GKI "protected export" check refuses unsigned modules the lookup functions of the backlight class.)
 */
#define TB_PWM_EN		0x00
#define TB_PWM_CON_1		0x1c
#define TB_PWM_APPLIED		0x2c
#define TB_PWM_BLS_DEBUG	0x80
#define TB_PWM_PERIOD_MASK	0xfffu
#define TB_PWM_HIGH_SHIFT	16
#define TB_PWM_HIGH_MASK	(0x1fffu << TB_PWM_HIGH_SHIFT)

static int tb330fu_bl_enable = 1;
module_param_named(backlight, tb330fu_bl_enable, int, 0444);
MODULE_PARM_DESC(backlight, "1 = register the backlight class device tb330fu_bl on the display PWM (DISP_PWM0)");
static unsigned long tb330fu_pwm_pa = 0x1100e000UL;
module_param_named(pwm_base, tb330fu_pwm_pa, ulong, 0444);
MODULE_PARM_DESC(pwm_base, "physical address of the display PWM block (DISP_PWM0)");

static void __iomem *tb330fu_pwm;
static struct backlight_device *tb330fu_bd;
static u32 tb330fu_pwm_period;
static u32 tb330fu_pwm_initial;
static bool tb330fu_screen_is_on = true;

static int tb330fu_bl_update(struct backlight_device *bd)
{
	/* backlight_get_brightness() is 0 while bl_power != FB_BLANK_UNBLANK: that is how the screen is switched off */
	u32 duty = min_t(u32, backlight_get_brightness(bd), tb330fu_pwm_period);

	writel((duty << TB_PWM_HIGH_SHIFT) | tb330fu_pwm_period, tb330fu_pwm + TB_PWM_CON_1);
	return 0;
}

static int tb330fu_bl_get(struct backlight_device *bd)
{
	/* what the panel really gets: 0 while the screen is off, so that user space (touch proxy, GNOME) can tell */
	return (readl(tb330fu_pwm + TB_PWM_APPLIED) & TB_PWM_HIGH_MASK) >> TB_PWM_HIGH_SHIFT;
}

static const struct backlight_ops tb330fu_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = tb330fu_bl_update,
	.get_brightness = tb330fu_bl_get,
};

static void tb330fu_bl_init(struct simpledrm_device *sdev)
{
	struct backlight_properties props = { };
	u32 con1, en;

	if (!tb330fu_bl_enable)
		return;
	tb330fu_pwm = ioremap(tb330fu_pwm_pa, 0x100);
	if (!tb330fu_pwm) {
		drm_info(&sdev->dev, "tb330fu: cannot map the display PWM, no backlight device\n");
		return;
	}
	en = readl(tb330fu_pwm + TB_PWM_EN);
	con1 = readl(tb330fu_pwm + TB_PWM_CON_1);
	tb330fu_pwm_period = con1 & TB_PWM_PERIOD_MASK;
	tb330fu_pwm_initial = (con1 & TB_PWM_HIGH_MASK) >> TB_PWM_HIGH_SHIFT;
	if (!(en & 1) || tb330fu_pwm_period < 16) {
		drm_info(&sdev->dev, "tb330fu: the display PWM is not running (EN=%#x CON_1=%#x), no backlight device\n", en, con1);
		goto unmap;
	}
	/* immediate mode: without it a write to CON_1 is not latched */
	writel(readl(tb330fu_pwm + TB_PWM_BLS_DEBUG) | 3, tb330fu_pwm + TB_PWM_BLS_DEBUG);
	props.type = BACKLIGHT_RAW;
	props.max_brightness = tb330fu_pwm_period;
	props.brightness = tb330fu_pwm_initial ? tb330fu_pwm_initial : tb330fu_pwm_period / 2;
	props.power = FB_BLANK_UNBLANK;
	tb330fu_bd = backlight_device_register("tb330fu_bl", NULL, NULL, &tb330fu_bl_ops, &props);
	if (IS_ERR(tb330fu_bd)) {
		drm_info(&sdev->dev, "tb330fu: backlight_device_register failed (%ld)\n", PTR_ERR(tb330fu_bd));
		tb330fu_bd = NULL;
		goto unmap;
	}
	drm_info(&sdev->dev, "tb330fu: backlight tb330fu_bl on DISP_PWM0 (period %u, LK left duty %u)\n", tb330fu_pwm_period, tb330fu_pwm_initial);
	return;
unmap:
	iounmap(tb330fu_pwm);
	tb330fu_pwm = NULL;
}

static void tb330fu_bl_exit(void)
{
	if (!tb330fu_bd)
		return;
	backlight_device_unregister(tb330fu_bd);
	tb330fu_bd = NULL;
	/* leave the panel lit at the level LK chose */
	writel(((tb330fu_pwm_initial ? tb330fu_pwm_initial : tb330fu_pwm_period / 2) << TB_PWM_HIGH_SHIFT) | tb330fu_pwm_period, tb330fu_pwm + TB_PWM_CON_1);
	iounmap(tb330fu_pwm);
	tb330fu_pwm = NULL;
}

static void tb330fu_screen_set(bool on)
{
	if (tb330fu_screen_is_on == on)
		return;
	tb330fu_screen_is_on = on;
	if (!tb330fu_bd)
		return;
	tb330fu_bd->props.power = on ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN;
	backlight_update_status(tb330fu_bd);
}

static struct iosys_map tb330fu_map[TB_MAXBUF];		/* [0] is sdev->screen_base (LK's buffer 0) */
static struct drm_rect tb330fu_dirty[TB_MAXBUF];	/* what each buffer still lacks of the newest frame */
static ktime_t tb330fu_free_at[TB_MAXBUF];		/* time after which the display engine is certainly done with the buffer */
static void __iomem *tb330fu_ovl_addr;
static bool tb330fu_flip_ok;
static int tb330fu_n;					/* buffers in the ring */
static int tb330fu_front;
static bool tb330fu_synced;
static u64 tb330fu_flips;

static u32 tb330fu_lk_addr(const struct simpledrm_device *sdev, unsigned long base)
{
	return (u32)(base + (unsigned long)sdev->pitch * (sdev->mode.vdisplay - 1) + 4ul * sdev->mode.hdisplay + 3);
}

static bool tb330fu_flip_active(void)
{
	return tb330fu_flip_ok && tb330fu_flip;
}

static void tb330fu_flip_init(struct simpledrm_device *sdev, struct platform_device *pdev, unsigned long buf0_pa)
{
	size_t size = (size_t)sdev->pitch * sdev->mode.vdisplay;
	void __iomem *page, *reg;
	u32 want, cur;
	int i, n = clamp(tb330fu_nbuf, 2, TB_MAXBUF);

	tb330fu_flip_ok = false;
	if (!tb330fu_flip) {
		drm_info(&sdev->dev, "tb330fu: flip=0, single-buffer scan-out\n");
		return;
	}
	if (sdev->format->cpp[0] != 4) {
		drm_info(&sdev->dev, "tb330fu: not a 32 bit format, single-buffer scan-out\n");
		return;
	}
	tb330fu_buf_pa[0] = buf0_pa;
	for (i = 1; i < n; i++) {
		void __iomem *b = devm_ioremap_wc(&pdev->dev, tb330fu_buf_pa[i], size);

		if (!b) {
			drm_info(&sdev->dev, "tb330fu: cannot map buffer %d (%#lx), single-buffer scan-out\n", i, tb330fu_buf_pa[i]);
			return;
		}
		iosys_map_set_vaddr_iomem(&tb330fu_map[i], b);
	}
	page = devm_ioremap(&pdev->dev, tb330fu_ovl_addr_pa & PAGE_MASK, PAGE_SIZE);
	if (!page) {
		drm_info(&sdev->dev, "tb330fu: cannot map the OVL register page, single-buffer scan-out\n");
		return;
	}
	reg = page + (tb330fu_ovl_addr_pa & ~PAGE_MASK);
	want = tb330fu_lk_addr(sdev, buf0_pa);
	cur = readl(reg);
	if (cur != want) {
		drm_info(&sdev->dev, "tb330fu: OVL address register is %08x, buffer 0 would be %08x: not flipping\n", cur, want);
		return;
	}
	tb330fu_map[0] = sdev->screen_base;
	tb330fu_ovl_addr = reg;
	tb330fu_n = n;
	tb330fu_front = 0;
	tb330fu_synced = false;
	memset(tb330fu_dirty, 0, sizeof(tb330fu_dirty));
	for (i = 0; i < TB_MAXBUF; i++)
		tb330fu_free_at[i] = ktime_get();
	tb330fu_flip_ok = true;
	drm_info(&sdev->dev, "tb330fu: scan-out ring of %d buffers: %#lx %#lx %#lx, OVL address register %#lx = %08x\n", n, tb330fu_buf_pa[0],
		 tb330fu_buf_pa[1], n > 2 ? tb330fu_buf_pa[2] : 0, tb330fu_ovl_addr_pa, cur);
}

static void tb330fu_rect_union(struct drm_rect *u, const struct drm_rect *r)
{
	if (r->x2 <= r->x1 || r->y2 <= r->y1)
		return;
	if (u->x2 <= u->x1 || u->y2 <= u->y1) {
		*u = *r;
		return;
	}
	u->x1 = min(u->x1, r->x1);
	u->y1 = min(u->y1, r->y1);
	u->x2 = max(u->x2, r->x2);
	u->y2 = max(u->y2, r->y2);
}

static void tb330fu_flip_to(struct simpledrm_device *sdev, int buf)
{
	wmb();
	writel(tb330fu_lk_addr(sdev, tb330fu_buf_pa[buf]), tb330fu_ovl_addr);
	tb330fu_front = buf;
}

static void tb330fu_flip_update(struct simpledrm_device *sdev, struct drm_plane_state *old_plane_state,
				struct drm_plane_state *plane_state, struct drm_shadow_plane_state *shadow_plane_state,
				struct drm_framebuffer *fb)
{
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect damage, cur = { 0, 0, 0, 0 }, full = { 0, 0, sdev->mode.hdisplay, sdev->mode.vdisplay };
	struct iosys_map dst;
	int i, old = tb330fu_front, back = (tb330fu_front + 1) % tb330fu_n;
	s64 us;

	drm_atomic_helper_damage_iter_init(&iter, old_plane_state, plane_state);
	drm_atomic_for_each_plane_damage(&iter, &damage) {
		struct drm_rect dst_clip = plane_state->dst;

		if (!drm_rect_intersect(&dst_clip, &damage))
			continue;
		tb330fu_rect_union(&cur, &dst_clip);
	}
	if (!tb330fu_synced) {
		/* the buffers do not yet hold the same picture: every buffer needs a full frame */
		cur = full;
		for (i = 0; i < tb330fu_n; i++)
			tb330fu_dirty[i] = full;
	} else if (cur.x2 <= cur.x1 || cur.y2 <= cur.y1) {
		return;
	} else {
		for (i = 0; i < tb330fu_n; i++)
			tb330fu_rect_union(&tb330fu_dirty[i], &cur);
	}

	/* the display engine may still scan the buffer we want to overwrite for up to one refresh period after it was flipped away from */
	us = ktime_us_delta(tb330fu_free_at[back], ktime_get());
	if (us > 0)
		usleep_range(us, us + 500);

	dst = tb330fu_map[back];
	iosys_map_incr(&dst, drm_fb_clip_offset(sdev->pitch, sdev->format, &tb330fu_dirty[back]));
	drm_fb_blit(&dst, &sdev->pitch, sdev->format->format, shadow_plane_state->data, fb, &tb330fu_dirty[back]);
	memset(&tb330fu_dirty[back], 0, sizeof(tb330fu_dirty[back]));

	tb330fu_flip_to(sdev, back);
	tb330fu_free_at[old] = ktime_add_us(ktime_get(), (s64)tb330fu_hold_ms * 1000);
	tb330fu_synced = true;
	tb330fu_flips++;
}

static void tb330fu_flip_clear(struct simpledrm_device *sdev)
{
	int i;

	if (!tb330fu_flip_ok)
		return;
	/* the plane is switched off: black in the other buffers as well, the next commit starts from a full frame */
	for (i = 1; i < tb330fu_n; i++)
		iosys_map_memset(&tb330fu_map[i], 0, 0, sdev->pitch * sdev->mode.vdisplay);
	tb330fu_synced = false;
}

static void tb330fu_flip_restore(struct simpledrm_device *sdev)
{
	tb330fu_bl_exit();
	if (!tb330fu_flip_ok)
		return;
	/* leave the scan-out where the boot console (and the next driver) expect it: buffer 0 */
	if (tb330fu_front)
		tb330fu_flip_to(sdev, 0);
	tb330fu_flip_ok = false;
}
