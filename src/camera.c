/* SPDX-License-Identifier: Apache-2.0 */
/* freebell-bk7258: camera: DVP open/close, encoder mode, scene hint + exposure settle, IR-cut, rails, RTSP callbacks, cam/rtsp commands. */
/* OV9650 on CPU1, frames to CPU0 via the media transfer task (split out of app_main.c in 0.28.0) */
#include "app_internal.h"

#if (CONFIG_SYS_CPU0)
#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
bool s_cam_open;   /* camera state, used by the light reading too */
#endif
#endif
#if (CONFIG_SYS_CPU0)
#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
/* ---- camera (AVDK build only): DVP sensor -> hardware JPEG on CPU1, frames delivered here by the
 * media transfer task; we keep the latest JPEG in PSRAM and can push it to a TCP peer ---- */
/* CPU1 never emits a JPEG larger than its own frame buffer (CONFIG_JPEG_FRAME_SIZE = 150 KB). The
 * CPU0 PSRAM heap is only 512 KB and also holds one such buffer per HTTP client plus the 128 KB
 * RTSP ring, so size this exactly. */
/* latest JPEG + its metadata: written by the frame callback, read by cam_http.c under s_cam_mutex */
uint8_t *s_cam_buf;
volatile uint32_t s_cam_len, s_cam_frames, s_cam_seq, s_cam_w, s_cam_h;
static volatile uint32_t s_cam_drop, s_cam_last_ms;
beken_mutex_t s_cam_mutex;

/* ---- encoder mode (v0.19.0): the DVP pipeline runs ONE hardware encoder at a time, JPEG (HTTP
 * snapshot/MJPEG) or H.264 (RTSP). Who gets it is decided by `cam mode` (KV db_cam_mode):
 *   auto (default): consumer driven. An RTSP client switches the camera to H.264; an HTTP request
 *                   switches it back to JPEG unless an RTSP client is playing (then the snapshot
 *                   is the last JPEG taken before the switch, /stream answers 503).
 *   jpeg:           never H.264 (RTSP DESCRIBE answers 503)   - the pre-0.19 behaviour.
 *   h264:           never JPEG (HTTP serves the stale JPEG / 503); use the RTSP stream in HA.
 * A switch costs a close + open (~0.5 s) and the first frame of the new mode (~1 s). ---- */
enum { CAM_PREF_AUTO = 0, CAM_PREF_JPEG = 1, CAM_PREF_H264 = 2 };
static int s_cam_pref = CAM_PREF_AUTO;
static bool s_cam_h264;                      /* mode of the open camera */
static volatile uint32_t s_cam_h264_frames;  /* H.264 access units since the last open */
static beken_mutex_t s_cam_ctl;              /* serialises open/close/switch (init in user_app_main) */
/* H.264 rate control (KV db_h264_q "IMB PMB"): target bits per 16x16 macroblock for I and P
 * frames. 720p = 3600 MB, GOP = 30 frames (CONFIG_H264_P_FRAME_CNT=29 on CPU1 = IDR every 2 s at
 * 15 fps): 120/24 ~ 54 KB I + 11 KB P = ~1.5 Mbit/s nominal, less on a static scene. The SDK's own
 * preset (quality level 2: 160/60) is ~3.5 Mbit/s. Keep IMB <= 290 (I-frame must fit the 128 KB
 * CPU1 buffer). */
static uint32_t s_h264_imb = 120, s_h264_pmb = 24;
static bool s_h264_q_pending;                /* apply after the first frame (the SDK preset is applied there) */
static uint16_t s_rtsp_port = RTSP_DEFAULT_PORT;
static bool s_rtsp_on;                       /* KV db_rtsp, default off */
#define KV_CAM_MODE "db_cam_mode"
#define KV_H264_Q "db_h264_q"
#define KV_RTSP "db_rtsp"
#define KV_RTSP_PORT "db_rtsp_port"

/* ---- scene hint + exposure settle gate (v0.27.0) ----
 * A cold OV9650 open in daylight used to deliver white frames for ~10 s (stock table: night mode
 * on, long initial exposure, bright AE target; see dvp_ov9650.c). Now every open tells CPU1 the
 * scene: the light sensor is read once the camera rail is up and `day` or `night` is ORed into
 * media_camera_device_t.info.fps (bits 6/7 of a bit-mask enum whose highest real value is FPS30 =
 * bit 5); dvp_camera.c copies the struct through the mailbox unchanged and calls the sensor's
 * set_fps() with it right after the init table, where the overlay driver programs the per-scene
 * registers. `cam scene auto|day|night|stock` (KV db_cam_scene) overrides the reading; `stock`
 * sends no hint = the pre-0.27 table only. The IR-cut coil (GPIO 54 = day, 55 = night, 300 ms
 * pulse, as the stock firmware does) is pulsed when the scene differs from the last one applied
 * (KV db_ircut), overlapping the rail/reset delays so the open is not slower.
 * The gate: JPEG consumers (HTTP snapshot, first MJPEG frame, SD save) wait after an open until the
 * AEC has settled: `settle N` frames since the open (default 8), or earlier once three consecutive
 * JPEGs differ by less than `settle PCT` (4 %) in size (a cheap exposure-change proxy), capped at
 * `settle MS` (2500) after the open so HA's 10 s snapshot timeout is never near. */
enum { CAM_SCENE_AUTO = 0, CAM_SCENE_DAY = 1, CAM_SCENE_NIGHT = 2, CAM_SCENE_STOCK = 3 };
#define OV9650_SCENE_DAY   (1u << 6)   /* same encoding as avdk-overlay/.../dvp_ov9650.c: byte-sized enum, */
#define OV9650_SCENE_NIGHT (1u << 7)   /* bit 2 = FPS15, bits 0-1/3-5 = 5-bit start-exposure class */
#define OV9650_EXP_ENCODE(c) (((((c) >> 2) & 7u) << 3) | ((c) & 3u))
#define KV_CAM_EXP "db_cam_exp"        /* 0 = class from the light reading, 1-11 = fixed start exposure 1 << (N-1) rows */
#define CAM_EXP_ROWS(c) ((c) >= 16 ? (1u << ((c) - 16 > 10 ? 10 : (c) - 16)) : (c) ? (1u << (((c) > 11 ? 11 : (c)) - 1)) : 4u)
#define KV_CAM_SCENE "db_cam_scene"
#define KV_CAM_SETTLE "db_cam_settle"   /* "N MS PCT" */
#define KV_IRCUT "db_ircut"             /* "day" | "night": the last IR-cut pulse applied */
#define PIN_IRCUT_DAY BOARD_PIN_IRCUT_DAY
#define PIN_IRCUT_NIGHT BOARD_PIN_IRCUT_NIGHT
#define IRCUT_PULSE_MS 300
static int s_cam_scene_pref = CAM_SCENE_AUTO;
static uint32_t s_cam_exp_fixed, s_cam_exp_class;   /* `cam exp N` override, and the class sent at the last open */
static int s_cam_scene;                 /* scene of the open camera: 0 stock/none, 1 day, 2 night */
static uint32_t s_cam_open_mv;          /* light sensor at the open */
static uint32_t s_cam_settle_n = 16, s_cam_settle_ms = 4500, s_cam_settle_pct = 4;   /* 2026-10-05: a manual-first day open is good from the first frame (the gate opens on size stability at N/2), an AEC-on start at ~4.3 s / ~18 frames */
static volatile uint32_t s_cam_open_frames, s_cam_prev_len, s_cam_stable, s_cam_settled_ms;
static volatile bool s_cam_settled = true;
static volatile char s_cam_settle_how = '-';   /* n = frame count, s = stable size, t = timeout */
static uint32_t s_cam_settle_wait_ms, s_cam_settle_waits;   /* the last wait a consumer spent in the gate, and how many waited */
static int s_ircut_state = -1;          /* -1 unknown, CAM_SCENE_DAY, CAM_SCENE_NIGHT */
static uint32_t s_ircut_t0;             /* pulse start (ms), 0 = no pulse in progress */
static int s_ircut_pin;
static uint32_t s_cam_off_ms, s_cam_open_ms;   /* last power-off and last open (ms) */
volatile bool s_cam_opening;      /* cam_open_locked() in progress: light_read_mv() must leave GPIO 28 alone */
volatile uint32_t s_light_last_mv;  /* last plausible light reading (>= 100 mV), fallback for the scene decision */

static void cam_frame_cb(frame_buffer_t *frame)
{
	if (!frame || !frame->frame || frame->length == 0)
		return;
	if (frame->fmt == PIXEL_FMT_H264) {   /* H.264 access unit (Annex-B) -> RTSP ring; nothing else wants it */
		s_cam_h264_frames++;
		s_cam_frames++;
		s_cam_last_ms = rtos_get_time();
		rtsp_feed_frame(frame->frame, frame->length, frame->timestamp);
		return;
	}
	if (frame->length > CAM_BUF_MAX || !s_cam_buf)
		return;
	if (rtos_trylock_mutex(&s_cam_mutex) != BK_OK) {   /* a snapshot is being read out: skip this frame */
		s_cam_drop++;
		return;
	}
	os_memcpy(s_cam_buf, frame->frame, frame->length);
	s_cam_len = frame->length;
	s_cam_seq = frame->sequence;
	s_cam_w = frame->width;
	s_cam_h = frame->height;
	s_cam_frames++;
	s_cam_last_ms = rtos_get_time();
	rtos_unlock_mutex(&s_cam_mutex);
	s_cam_open_frames++;
	if (!s_cam_settled) {   /* settle gate: N frames, or three JPEGs in a row within PCT % of each other */
		uint32_t prev = s_cam_prev_len, len = frame->length, d = prev > len ? prev - len : len - prev;

		s_cam_stable = (prev && d * 100 < s_cam_settle_pct * len) ? s_cam_stable + 1 : 0;
		s_cam_prev_len = len;
		if (s_cam_open_frames >= s_cam_settle_n)
			s_cam_settle_how = 'n';
		else if (s_cam_open_frames >= s_cam_settle_n / 2 && s_cam_open_frames >= 3 && s_cam_stable >= 2)
			s_cam_settle_how = 's';
		else
			return;
		s_cam_settled_ms = s_cam_last_ms - s_cam_open_ms;
		s_cam_settled = true;
	}
}

static void camsnap_task(beken_thread_arg_t arg)
{
	int sock = rig_tcp_connect(s_net_host, s_net_port);
	uint32_t len = 0, off = 0;
	int n;

	if (sock < 0) {
		os_printf("[cam] connect %s:%u failed\r\n", s_net_host, s_net_port);
		goto out;
	}
	rtos_lock_mutex(&s_cam_mutex);
	len = s_cam_len;
	while (off < len && (n = send(sock, s_cam_buf + off, len - off > 4096 ? 4096 : len - off, 0)) > 0)
		off += n;
	rtos_unlock_mutex(&s_cam_mutex);
	close(sock);
	os_printf("[cam] snapshot %u/%u bytes -> %s:%u\r\n", off, len, s_net_host, s_net_port);
out:
	rtos_delete_thread(NULL);
}


#endif
#endif
#if (CONFIG_SYS_CPU0)
#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
#if CONFIG_DVP_CAMERA
/* GPIO 6 used to be in this list as a "camera LDO enable": it is the microSD card-detect input
 * (stock firmware, verified 2026-10-04) and belongs to sdcard.c now. */
void cam_rails(int on)
{
	static const int rails[] = {BOARD_PIN_SENSOR_RAIL, BOARD_PIN_CAM_PWR, BOARD_PIN_CAM_LDO};
	for (unsigned i = 0; i < sizeof(rails) / sizeof(rails[0]); i++) {
		pin_configure(rails[i], GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
		if (on) bk_gpio_set_output_high(rails[i]); else bk_gpio_set_output_low(rails[i]);
	}
}

/* The camera's own supplies: GPIO 28 (sensor power, also feeds the LDR divider) and the LDO
 * enable 5. Dropped at every close so the next open is a real power-on reset of the OV9650:
 * a close followed by a reopen within a few seconds otherwise leaves the sensor silent (no
 * frames, "camera wedge", 2026-10-03) and only a rail cycle or a reboot brings it back. GPIO 9
 * (light sensor / LED rail) is left alone. */
static void cam_power(int on)   /* s_cam_off_ms / s_cam_open_ms live with the settle state above */
{
	static const int pins[] = {BOARD_PIN_CAM_PWR, BOARD_PIN_CAM_LDO};
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		pin_configure(pins[i], GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
		if (on) bk_gpio_set_output_high(pins[i]); else bk_gpio_set_output_low(pins[i]);
	}
	if (!on)
		s_cam_off_ms = rtos_get_time();
}

#endif
#endif
#endif
#if (CONFIG_SYS_CPU0)
#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
#if CONFIG_DVP_CAMERA
/* IR-cut filter coil: GPIO 54 pulsed high moves it to the day position, GPIO 55 to night (stock
 * firmware: 300 ms). Only pulsed when the scene differs from the last pulse (KV db_ircut), and
 * the pulse overlaps the open's own delays: ircut_begin() raises the pin, ircut_end() waits out
 * whatever is left of the 300 ms and drops it. `cam ircut day|night` forces one. */
static void ircut_begin(int scene)
{
	const char *name = scene == CAM_SCENE_DAY ? "day" : "night";

	s_ircut_pin = scene == CAM_SCENE_DAY ? PIN_IRCUT_DAY : PIN_IRCUT_NIGHT;
	pin_configure(PIN_IRCUT_DAY, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
	pin_configure(PIN_IRCUT_NIGHT, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
	bk_gpio_set_output_low(PIN_IRCUT_DAY);
	bk_gpio_set_output_low(PIN_IRCUT_NIGHT);
	bk_gpio_set_output_high(s_ircut_pin);
	s_ircut_t0 = rtos_get_time() | 1;
	s_ircut_state = scene;
	kv_set_str(KV_IRCUT, name);
	os_printf("[cam] ir-cut -> %s (GPIO %d pulse)\r\n", name, s_ircut_pin);
}

static void ircut_end(void)
{
	uint32_t el;

	if (!s_ircut_t0)
		return;
	el = rtos_get_time() - s_ircut_t0;
	if (el < IRCUT_PULSE_MS)
		rtos_delay_milliseconds(IRCUT_PULSE_MS - el);
	bk_gpio_set_output_low(s_ircut_pin);
	s_ircut_t0 = 0;
}
#endif /* CONFIG_DVP_CAMERA */

/* Start-exposure class for a day open from the light sensor: class 1..11 = 1 << (class - 1) rows
 * (one row ~127 us) with the AEC on at once, class 16..26 = the same rows manual-first. The LDR is
 * a weak illuminance meter (663 mV on a sunny morning reads like a lit room, yet the AEC settled at
 * 1 row, gain 1x) and the loop moves about a stop per 0.5 s either way, so err short: a dark first
 * frame keeps its detail, a white one does not. `cam exp N` fixes a class; the CPU1 trace
 * (camera.md, "Checking CPU1 from CPU0") shows where the AEC ends up for a given reading. */
static uint32_t cam_exp_class(uint32_t mv)
{
	if (s_cam_exp_fixed)
		return s_cam_exp_fixed;
	/* Default = manual-first (class 16+): AEC/AGC stay off for the first second at this exposure,
	 * then take over. Measured 2026-10-05 (sunny, 515-663 mV): the AEC settles at 1 row, gain 1x;
	 * started there the first frame is right and the AEC stays put, whereas any AEC-on start
	 * overshoots to ~130 rows on the dark start-up frames and needs ~4 s to come back. */
	if (mv < 700) return 16;    /* 1 row */
	if (mv < 850) return 18;    /* 4 */
	if (mv < 1000) return 20;   /* 16 */
	if (mv < 1200) return 22;   /* 64 */
	return 24;                  /* 256 */
}

static const char *cam_scene_name(int scene)
{
	static const char *const names[] = {"auto", "day", "night", "stock"};
	return scene >= 0 && scene <= 3 ? names[scene] : "?";
}

/* Open the sensor in JPEG or H.264 mode (1280x720 only for H.264: the encoder needs height % 16
 * == 0 and the OV9650 table has 720p). Caller holds s_cam_ctl. */
static int cam_open_locked(uint32_t w, uint32_t h, bool h264)
{
	media_camera_device_t dev = {0};
	int ret, scene = s_cam_scene_pref;
	uint32_t mv = 0;

	if (s_cam_open)
		return s_cam_h264 == h264 ? BK_OK : BK_ERR_STATE;
	if (!s_cam_buf)
		s_cam_buf = psram_malloc(CAM_BUF_MAX);
	if (!s_cam_buf)
		return BK_ERR_NO_MEM;
	if (!s_cam_mutex)
		rtos_init_mutex(&s_cam_mutex);
	s_cam_opening = true;
#if CONFIG_DVP_CAMERA
	if (s_cam_off_ms) {   /* a real power-off: 250 ms left 3/8 cold opens silent or undetected */
		uint32_t off = rtos_get_time() - s_cam_off_ms;
		if (off < 800)
			rtos_delay_milliseconds(800 - off);
	}
	cam_rails(1);   /* GPIO 9, 28 (camera power, active high per the stock firmware), 5 */
	rtos_delay_milliseconds(150);
	/* GPIO 28 is up, so the LDR divider is live and this is a plain ADC read. < 100 mV is the
	 * unpowered-divider signature (seen once at a cold wake, 2026-10-05: the wake report's
	 * light_read_mv() in the reporter thread dropped GPIO 28 under us, hence s_cam_opening);
	 * re-read a few times, then fall back to the last plausible reading. */
	for (int t = 0; t < 4; t++) {
		mv = adc_read_mv(LIGHT_ADC_CHAN);
		if (mv >= 100)
			break;
		rtos_delay_milliseconds(30);
	}
	if (mv < 100 && s_light_last_mv) {
		os_printf("[cam] light read %u mV (divider off?), using last %u mV\r\n", (unsigned)mv, (unsigned)s_light_last_mv);
		mv = s_light_last_mv;
	} else if (mv >= 100) {
		s_light_last_mv = mv;
	}
	if (scene == CAM_SCENE_AUTO)
		scene = mv > s_dark_mv ? CAM_SCENE_NIGHT : CAM_SCENE_DAY;
	if ((scene == CAM_SCENE_DAY || scene == CAM_SCENE_NIGHT) && scene != s_ircut_state)
		ircut_begin(scene);   /* overlaps the 250 ms reset pulse below; ircut_end() pads to 300 ms */
	/* stock bk_dvp_camera_enumerate: GPIO 52 low 100 ms, high 100 ms, low (sensor PWDN/RESET pulse) */
	pin_configure(BOARD_PIN_CAM_PWDN, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
	bk_gpio_set_output_low(BOARD_PIN_CAM_PWDN);
	rtos_delay_milliseconds(100);
	bk_gpio_set_output_high(BOARD_PIN_CAM_PWDN);
	rtos_delay_milliseconds(100);
	bk_gpio_set_output_low(BOARD_PIN_CAM_PWDN);
	rtos_delay_milliseconds(50);
	ircut_end();
#else
	if (scene == CAM_SCENE_AUTO)
		scene = CAM_SCENE_DAY;
#endif
	dev.type = DVP_CAMERA;
	dev.mode = h264 ? H264_MODE : JPEG_MODE;
	dev.fmt = h264 ? PIXEL_FMT_H264 : PIXEL_FMT_JPEG;
	dev.info.resolution.width = w;
	dev.info.resolution.height = h;
	/* scene hint for the CPU1 sensor driver, see the block comment above cam_frame_cb() */
	s_cam_exp_class = scene == CAM_SCENE_DAY ? cam_exp_class(mv) : 0;
	dev.info.fps = (frame_fps_t)(FPS15 | (scene == CAM_SCENE_DAY ? (OV9650_SCENE_DAY | OV9650_EXP_ENCODE(s_cam_exp_class)) :
					     scene == CAM_SCENE_NIGHT ? OV9650_SCENE_NIGHT : 0));
	s_cam_scene = scene == CAM_SCENE_STOCK ? 0 : scene;
	s_cam_open_mv = mv;
	s_cam_h264_frames = 0;
	s_cam_open_frames = 0;
	s_cam_prev_len = 0;
	s_cam_stable = 0;
	s_cam_settled_ms = 0;
	s_cam_settle_how = '-';
	s_cam_settled = h264;   /* the gate is for JPEG consumers only */
	ret = media_app_camera_open(&dev);
	if (ret != BK_OK) {
#if CONFIG_DVP_CAMERA
		cam_power(0);   /* undetected cold sensor: make the retry a real power cycle */
#endif
		s_cam_opening = false;
		return ret;
	}
	/* one registration at a time: the format selects which CPU1 frame queue is forwarded */
	ret = media_app_register_read_frame_callback(dev.fmt, cam_frame_cb);
	s_cam_open = (ret == BK_OK);
	s_cam_h264 = s_cam_open && h264;
	s_h264_q_pending = s_cam_h264;
	s_cam_open_ms = rtos_get_time();
	s_cam_last_ms = s_cam_open_ms;
	s_cam_opening = false;
	os_printf("[cam] open %ux%u %s scene %s (%u mV, exp class %u) -> %d\r\n", w, h, h264 ? "h264" : "jpeg", cam_scene_name(s_cam_scene), (unsigned)mv, (unsigned)s_cam_exp_class, ret);
	return ret;
}

/* Exposure settle gate (see the block above cam_frame_cb): block a JPEG consumer until the AEC
 * has settled after the open, at most `settle MS` after the open. Returns the ms spent here. */
uint32_t cam_settle_wait(void)
{
	uint32_t t0 = rtos_get_time(), waited;

	if (!s_cam_open || s_cam_h264 || s_cam_settled)
		return 0;
	while (!s_cam_settled && s_cam_open && !s_cam_h264) {
		if (rtos_get_time() - s_cam_open_ms >= s_cam_settle_ms) {
			s_cam_settled_ms = rtos_get_time() - s_cam_open_ms;
			s_cam_settle_how = 't';
			s_cam_settled = true;
			break;
		}
		rtos_delay_milliseconds(20);
	}
	waited = rtos_get_time() - t0;
	s_cam_settle_wait_ms = waited;
	s_cam_settle_waits++;
	return waited;
}

/* Caller holds s_cam_ctl. The last JPEG stays in s_cam_buf (stale snapshot while in H.264). */
static void cam_close_locked(void)
{
	int ret;

	media_app_unregister_read_frame_callback();
	ret = media_app_camera_close(DVP_CAMERA);
	os_printf("[cam] close (%s) -> %d\r\n", s_cam_h264 ? "h264" : "jpeg", ret);
	s_cam_open = false;
	s_cam_h264 = false;
	s_h264_q_pending = false;
#if CONFIG_DVP_CAMERA
	cam_power(0);   /* power-on reset at the next open, see cam_power() */
#endif
}

/* Push our H.264 rate-control targets to the CPU1 encoder. Only valid once the first frame is
 * out: bk_h264_init() queues the SDK preset and the encoder ISR applies it at the first frame,
 * and bk_h264_set_base_config() refuses while that is pending. */
static int cam_h264_apply_q(void)
{
	compress_ratio_t r = {0};
	int ret = BK_FAIL, tries;

	if (!s_cam_open || !s_cam_h264)
		return BK_ERR_STATE;
	r.mode = H264_MODE;
	r.enable = 1;
	r.qp.init_qp = 30;
	r.qp.i_min_qp = 10;
	r.qp.i_max_qp = 45;
	r.qp.p_min_qp = 10;
	r.qp.p_max_qp = 45;
	r.imb_bits = s_h264_imb;
	r.pmb_bits = s_h264_pmb;
	for (tries = 0; tries < 3 && ret != BK_OK; tries++) {
		ret = media_app_set_compression_ratio(&r);
		if (ret != BK_OK)
			rtos_delay_milliseconds(150);
	}
	os_printf("[cam] h264 q imb %u pmb %u -> %d\r\n", s_h264_imb, s_h264_pmb, ret);
	return ret;
}

/* Caller holds s_cam_ctl. A cold OV9650 sometimes comes up silent (open -> 0, never a frame):
 * treat "open for 4 s and no frame for 4 s" as stalled and power-cycle it before use. */
static bool cam_stalled_locked(void)
{
	uint32_t now = rtos_get_time();

	if (!s_cam_open || now - s_cam_open_ms < 4000 || now - s_cam_last_ms < 4000)
		return false;
	os_printf("[cam] stalled (%s, no frame for %u ms): power cycle\r\n", s_cam_h264 ? "h264" : "jpeg", (unsigned)(now - s_cam_last_ms));
	cam_close_locked();
	return true;
}

/* HTTP wants JPEG frames. 0 = the camera is (now) in JPEG mode, -1 = stay/503 (see cam mode). */
int cam_want_jpeg(void)
{
	int ret = 0, playing = 0;

	rtos_lock_mutex(&s_cam_ctl);
	cam_stalled_locked();
	if (s_cam_open && !s_cam_h264)
		goto out;
	if (s_cam_pref == CAM_PREF_H264) {
		ret = -1;
		goto out;
	}
	if (s_cam_open && s_cam_h264) {
		rtsp_clients(&playing);
		if (playing > 0) {   /* auto: a live RTSP viewer keeps H.264 */
			ret = -1;
			goto out;
		}
		cam_close_locked();
	}
	ret = cam_open_locked(1280, 720, false);
out:
	rtos_unlock_mutex(&s_cam_ctl);
	return ret;
}

/* ---- rtsp.c callbacks: the RTSP server wants H.264 ---- */
int rtsp_cam_acquire(void)
{
	int ret = 0, w;

	rtos_lock_mutex(&s_cam_ctl);
	if (s_cam_pref == CAM_PREF_JPEG) {
		ret = -1;
		goto out;
	}
	cam_stalled_locked();
	for (int attempt = 0; attempt < 2; attempt++) {   /* a silent or undetected cold sensor gets one power cycle */
		if (s_cam_open && !s_cam_h264)
			cam_close_locked();
		if (!s_cam_open)
			ret = cam_open_locked(1280, 720, true);
		if (ret == 0) {
			for (w = 0; w < 300 && s_cam_h264_frames == 0; w++)   /* first access unit ~1 s after open */
				rtos_delay_milliseconds(10);
			if (s_cam_h264_frames)
				break;
			os_printf("[cam] h264: no frame within 3 s\r\n");
			ret = -2;
		}
		if (s_cam_open)
			cam_close_locked();
	}
	if (ret != 0)
		goto out;
	if (s_h264_q_pending) {
		rtos_delay_milliseconds(100);   /* let the encoder ISR consume the SDK preset first */
		if (cam_h264_apply_q() == BK_OK)
			s_h264_q_pending = false;
	}
out:
	rtos_unlock_mutex(&s_cam_ctl);
	return ret;
}

void rtsp_cam_release(void)
{
	rtos_lock_mutex(&s_cam_ctl);
	if (s_cam_open && s_cam_h264)
		cam_close_locked();
	rtos_unlock_mutex(&s_cam_ctl);
}

void rtsp_cam_idr(void)
{
	if (s_cam_open && s_cam_h264)
		media_app_h264_regenerate_idr(DVP_CAMERA);
}

#endif
#endif
#if (CONFIG_SYS_CPU0)
#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
int cam_copy_latest(uint8_t *dst, uint32_t cap, uint32_t *seq)
{
	uint32_t len;
	rtos_lock_mutex(&s_cam_mutex);
	len = s_cam_len;
	if (len && len <= cap) {
		os_memcpy(dst, s_cam_buf, len);
		*seq = s_cam_seq;
	} else {
		len = 0;
	}
	rtos_unlock_mutex(&s_cam_mutex);
	return (int)len;
}

/* For sdcard.c: latest JPEG, opening the camera on demand (first frame takes ~1 s). */
/* For sdcard.c: the microSD slot is on the camera supply and the SDK card init only completes with
 * the media pipeline up (2026-10-04), so a mount brings the JPEG camera up first. */
int app_sd_cam_ready(void)
{
	return cam_want_jpeg();
}

bool app_cam_is_open(void)
{
	return s_cam_open;
}

int app_cam_grab(uint8_t *dst, uint32_t cap, uint32_t wait_ms)
{
	uint32_t seq = 0, t0 = rtos_get_time();
	int n;

	if (cam_want_jpeg() != 0 && !s_cam_len)   /* H.264 busy (RTSP viewer): fall back to the last JPEG if any */
		return -1;
	if (!s_cam_mutex || !s_cam_buf)
		return -2;
	cam_settle_wait();   /* a cold open: let the exposure converge before the card gets the frame */
	while ((n = cam_copy_latest(dst, cap, &seq)) <= 0 && rtos_get_time() - t0 < wait_ms)
		rtos_delay_milliseconds(50);
	return n;
}

#endif
#endif
#if (CONFIG_SYS_CPU0)
#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
void cli_cam_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
#if CONFIG_DVP_CAMERA
	if (argc >= 2 && os_strcmp(argv[1], "probe") == 0) {
#if DOORBELL_BRINGUP
		cli_cam_probe(argc, argv);
#else
		REPLY("[cam] probe: bring-up build only (DOORBELL_BRINGUP 1 in features.h)\r\n");
#endif
		return;
	}
#endif
	if (argc >= 2 && os_strcmp(argv[1], "open") == 0) {
		/* cam open | open h264 | open W H | open W H h264 */
		bool h264 = os_strcmp(argv[argc - 1], "h264") == 0;
		int dims = argc - 2 - (h264 ? 1 : 0), ret;
		uint32_t w = dims >= 2 ? os_strtoul(argv[2], NULL, 10) : 1280, h = dims >= 2 ? os_strtoul(argv[3], NULL, 10) : 720;
		rtos_lock_mutex(&s_cam_ctl);
		if (s_cam_open && s_cam_h264 != h264)
			cam_close_locked();
		ret = cam_open_locked(w, h, h264);
		rtos_unlock_mutex(&s_cam_ctl);
		REPLY("[cam] open %ux%u %s -> %d%s\r\n", w, h, h264 ? "h264" : "jpeg", ret, s_cam_open ? " (open)" : "");
	} else if (argc >= 2 && os_strcmp(argv[1], "close") == 0) {
		rtos_lock_mutex(&s_cam_ctl);
		cam_close_locked();
		rtos_unlock_mutex(&s_cam_ctl);
		REPLY("[cam] closed\r\n");
	} else if (argc >= 2 && os_strcmp(argv[1], "stat") == 0) {
		static const char *const pref[] = {"auto", "jpeg", "h264"};
		REPLY("[cam] open %d %s (mode %s) frames %u h264 %u dropped %u last jpeg %u bytes %ux%u seq %u age %u ms"
		      " | scene %s (%s, %u mV, exp %u) open_frames %u settled %c@%u ms wait %u ms x%u settle %u/%u ms/%u%% ircut %s\r\n", s_cam_open,
		      s_cam_open ? (s_cam_h264 ? "h264" : "jpeg") : "-", pref[s_cam_pref], s_cam_frames, s_cam_h264_frames,
		      s_cam_drop, s_cam_len, s_cam_w, s_cam_h, s_cam_seq, s_cam_last_ms ? rtos_get_time() - s_cam_last_ms : 0,
		      s_cam_scene ? cam_scene_name(s_cam_scene) : "stock", cam_scene_name(s_cam_scene_pref), s_cam_open_mv, s_cam_exp_class, s_cam_open_frames,
		      s_cam_settle_how, s_cam_settled_ms, s_cam_settle_wait_ms, s_cam_settle_waits, s_cam_settle_n, s_cam_settle_ms, s_cam_settle_pct,
		      s_ircut_state < 0 ? "unknown" : cam_scene_name(s_ircut_state));
	} else if (argc >= 2 && os_strcmp(argv[1], "settle") == 0) {   /* cam settle [N [MS [PCT]]] */
		char buf[32];
		if (argc >= 3) {
			uint32_t n = os_strtoul(argv[2], NULL, 10), ms = argc >= 4 ? os_strtoul(argv[3], NULL, 10) : s_cam_settle_ms;
			uint32_t pct = argc >= 5 ? os_strtoul(argv[4], NULL, 10) : s_cam_settle_pct;
			if (n < 1 || n > 60 || ms < 100 || ms > 8000 || pct < 1 || pct > 50) {   /* cap: HA's snapshot timeout is 10 s incl. ~1 s open */
				REPLY("cam settle N(1-60) [MS(100-8000) [PCT(1-50)]]\r\n");
				return;
			}
			s_cam_settle_n = n;
			s_cam_settle_ms = ms;
			s_cam_settle_pct = pct;
			snprintf(buf, sizeof(buf), "%u %u %u", n, ms, pct);
			kv_set_str(KV_CAM_SETTLE, buf);
		}
		REPLY("[cam] settle %u frames, cap %u ms, stable < %u %% (applies at the next open)\r\n", s_cam_settle_n, s_cam_settle_ms, s_cam_settle_pct);
	} else if (argc >= 2 && os_strcmp(argv[1], "scene") == 0) {   /* cam scene auto|day|night|stock */
		if (argc >= 3) {
			int sc = os_strcmp(argv[2], "auto") == 0 ? CAM_SCENE_AUTO : os_strcmp(argv[2], "day") == 0 ? CAM_SCENE_DAY :
				 os_strcmp(argv[2], "night") == 0 ? CAM_SCENE_NIGHT : os_strcmp(argv[2], "stock") == 0 ? CAM_SCENE_STOCK : -1;
			if (sc < 0) {
				REPLY("cam scene auto|day|night|stock\r\n");
				return;
			}
			s_cam_scene_pref = sc;
			kv_set_str(KV_CAM_SCENE, argv[2]);
		}
		REPLY("[cam] scene %s (applies at the next open; light now %u mV, dark > %u; open camera: %s)\r\n", cam_scene_name(s_cam_scene_pref),
		      light_read_mv(), s_dark_mv, s_cam_open ? (s_cam_scene ? cam_scene_name(s_cam_scene) : "stock") : "-");
	} else if (argc >= 2 && os_strcmp(argv[1], "exp") == 0) {   /* cam exp [N]: day start exposure class, 0 = from the light */
		char buf[8];
		if (argc >= 3) {
			uint32_t n = os_strtoul(argv[2], NULL, 10);
			if (n > 26 || (n > 11 && n < 16)) {
				REPLY("cam exp 0-11 (0 = from the light sensor, else 1 << (N-1) rows; 16-26 = manual 1 << (N-16) rows for 1 s, then AEC)\r\n");
				return;
			}
			s_cam_exp_fixed = n;
			snprintf(buf, sizeof(buf), "%u", n);
			kv_set_str(KV_CAM_EXP, buf);
		}
		{
			uint32_t now_mv = light_read_mv(), c = cam_exp_class(now_mv);
			REPLY("[cam] exp class %u (%s; light now %u mV -> class %u = %u rows; applies at the next day open)\r\n", s_cam_exp_fixed,
			      s_cam_exp_fixed ? "fixed" : "auto", now_mv, c, CAM_EXP_ROWS(c));
		}
	} else if (argc >= 3 && os_strcmp(argv[1], "ircut") == 0 && (os_strcmp(argv[2], "day") == 0 || os_strcmp(argv[2], "night") == 0)) {
#if CONFIG_DVP_CAMERA
		ircut_begin(os_strcmp(argv[2], "day") == 0 ? CAM_SCENE_DAY : CAM_SCENE_NIGHT);
		ircut_end();
		REPLY("[cam] ir-cut pulsed to %s (remembered)\r\n", argv[2]);
#else
		REPLY("[cam] ir-cut: no DVP camera in this build\r\n");
#endif
	} else if (argc >= 3 && os_strcmp(argv[1], "mode") == 0) {
		if (os_strcmp(argv[2], "auto") == 0)
			s_cam_pref = CAM_PREF_AUTO;
		else if (os_strcmp(argv[2], "jpeg") == 0)
			s_cam_pref = CAM_PREF_JPEG;
		else if (os_strcmp(argv[2], "h264") == 0)
			s_cam_pref = CAM_PREF_H264;
		else {
			REPLY("cam mode auto|jpeg|h264\r\n");
			return;
		}
		kv_set_str(KV_CAM_MODE, argv[2]);
		REPLY("[cam] mode %s (applies at the next open/switch)\r\n", argv[2]);
	} else if (argc >= 4 && os_strcmp(argv[1], "snap") == 0) {
		beken_thread_t t;
		if (!s_cam_len) { REPLY("[cam] no frame yet\r\n"); return; }
		os_strncpy(s_net_host, argv[2], sizeof(s_net_host) - 1);
		s_net_port = os_strtoul(argv[3], NULL, 10);
		REPLY("[cam] sending %u bytes to %s:%u\r\n", s_cam_len, s_net_host, s_net_port);
		if (rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "camsnap", camsnap_task, 4096, NULL) != BK_OK)
			REPLY("[cam] thread failed\r\n");
	} else {
		REPLY("cam open [W H] [h264] | close | stat | mode auto|jpeg|h264 | settle [N [MS [PCT]]] | scene auto|day|night|stock | exp [0-11] | ircut day|night | snap HOST PORT | http://IP:8080/snapshot.jpg /stream | probe [PWR_PIN ...] | probe scan | probe rails on|off | probe bb SCL SDA\r\n");
	}
}

/* ---- rtsp: H.264 live stream server (rtsp.c), rtsp://IP:554/live ---- */
void cli_rtsp_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	char buf[320];

	if (argc >= 2 && os_strcmp(argv[1], "on") == 0) {
		int ret = rtsp_start(s_rtsp_port);
		s_rtsp_on = true;
		kv_set_str(KV_RTSP, "1");
		REPLY("[rtsp] on, port %u -> %d (persisted; clients switch the camera to H.264 on demand)\r\n", s_rtsp_port, ret);
	} else if (argc >= 2 && os_strcmp(argv[1], "off") == 0) {
		rtsp_stop();
		s_rtsp_on = false;
		kv_set_str(KV_RTSP, "0");
		REPLY("[rtsp] off (persisted; server stops within ~1 s, camera released if in H.264)\r\n");
	} else if (argc >= 2 && os_strcmp(argv[1], "status") == 0) {
		rtsp_status(buf, sizeof(buf));
		REPLY("%s", buf);
		rtsp_status_audio(buf, sizeof(buf));
		REPLY("%s", buf);
		if (rtsp_status_clients(buf, sizeof(buf)) > 0)
			REPLY("%s", buf);
	} else if (argc >= 3 && os_strcmp(argv[1], "port") == 0) {
		uint32_t p = os_strtoul(argv[2], NULL, 10);
		if (p == 0 || p > 65535) {
			REPLY("[rtsp] bad port\r\n");
			return;
		}
		s_rtsp_port = (uint16_t)p;
		kv_set_str(KV_RTSP_PORT, argv[2]);
		REPLY("[rtsp] port %u (takes effect at the next `rtsp off` + `rtsp on`, or reboot)\r\n", s_rtsp_port);
	} else if (argc >= 4 && os_strcmp(argv[1], "q") == 0) {
		uint32_t imb = os_strtoul(argv[2], NULL, 10), pmb = os_strtoul(argv[3], NULL, 10);
		if (imb < 10 || imb > 290 || pmb < 4 || pmb > 1000) {
			REPLY("[rtsp] q IMB(10..290) PMB(4..1000) bits per macroblock; 720p = 3600 MB, 15 fps, IDR every 2 s\r\n");
			return;
		}
		s_h264_imb = imb;
		s_h264_pmb = pmb;
		snprintf(buf, sizeof(buf), "%u %u", imb, pmb);
		kv_set_str(KV_H264_Q, buf);
		s_h264_q_pending = true;
		if (s_cam_open && s_cam_h264 && s_cam_h264_frames) {
			rtos_lock_mutex(&s_cam_ctl);
			if (cam_h264_apply_q() == BK_OK)
				s_h264_q_pending = false;
			rtos_unlock_mutex(&s_cam_ctl);
		}
		REPLY("[rtsp] q imb %u pmb %u (~%u kbit/s nominal)%s\r\n", imb, pmb, (imb + 29 * pmb) * 3600 / 2000,
		      s_h264_q_pending ? ", applied at the next H.264 open" : ", applied");
	} else if (argc >= 2 && os_strcmp(argv[1], "idr") == 0) {
		rtsp_request_idr();
		REPLY("[rtsp] IDR requested\r\n");
	} else {
		REPLY("rtsp on|off | status | port N | q IMB PMB | idr   (now %s, port %u, q %u/%u, cam mode %d)\r\n",
		      rtsp_running() ? "running" : "stopped", s_rtsp_port, s_h264_imb, s_h264_pmb, s_cam_pref);
	}
}
#endif
#endif

#if defined(DOORBELL_AVDK) && (CONFIG_SYS_CPU0)
/* ---- module API ---- */
void camera_init(void)   /* KV settings, the HTTP server, RTSP when enabled */
{
	char v[24], *e;

	rtos_init_mutex(&s_cam_ctl);
	if (kv_get_str(KV_CAM_MODE, v, sizeof(v)))
		s_cam_pref = os_strcmp(v, "jpeg") == 0 ? CAM_PREF_JPEG : os_strcmp(v, "h264") == 0 ? CAM_PREF_H264 : CAM_PREF_AUTO;
	if (kv_get_str(KV_H264_Q, v, sizeof(v))) {
		uint32_t a = os_strtoul(v, &e, 10), b = os_strtoul(e, NULL, 10);
		if (a >= 10 && a <= 290 && b >= 4 && b <= 1000) {
			s_h264_imb = a;
			s_h264_pmb = b;
		}
	}
	if (kv_get_str(KV_RTSP_PORT, v, sizeof(v)) && os_strtoul(v, NULL, 10) && os_strtoul(v, NULL, 10) <= 65535)
		s_rtsp_port = (uint16_t)os_strtoul(v, NULL, 10);
	if (kv_get_str(KV_RTSP, v, sizeof(v)))
		s_rtsp_on = v[0] == '1';
	if (kv_get_str(KV_CAM_SETTLE, v, sizeof(v))) {   /* "N MS PCT" */
		uint32_t n = os_strtoul(v, &e, 10), ms = os_strtoul(e, &e, 10), pct = os_strtoul(e, NULL, 10);
		if (n >= 1 && n <= 60 && ms >= 100 && ms <= 8000 && pct >= 1 && pct <= 50) {
			s_cam_settle_n = n;
			s_cam_settle_ms = ms;
			s_cam_settle_pct = pct;
		}
	}
	if (kv_get_str(KV_CAM_SCENE, v, sizeof(v)))
		s_cam_scene_pref = os_strcmp(v, "day") == 0 ? CAM_SCENE_DAY : os_strcmp(v, "night") == 0 ? CAM_SCENE_NIGHT :
				   os_strcmp(v, "stock") == 0 ? CAM_SCENE_STOCK : CAM_SCENE_AUTO;
	if (kv_get_str(KV_CAM_EXP, v, sizeof(v)) && os_strtoul(v, NULL, 10) <= 26)
		s_cam_exp_fixed = os_strtoul(v, NULL, 10);
	if (kv_get_str(KV_IRCUT, v, sizeof(v)))
		s_ircut_state = os_strcmp(v, "day") == 0 ? CAM_SCENE_DAY : os_strcmp(v, "night") == 0 ? CAM_SCENE_NIGHT : -1;
	cam_http_start();
	if (s_rtsp_on)   /* default off: nothing changes for the camera until `rtsp on` */
		rtsp_start(s_rtsp_port);
}

/* before deep sleep: close through the camera owner's lock so an RTSP acquire in flight cannot reopen it half-way */
void camera_close_for_sleep(void)
{
	if (s_cam_open) {
		rtos_lock_mutex(&s_cam_ctl);
		cam_close_locked();
		rtos_unlock_mutex(&s_cam_ctl);
		rtos_delay_milliseconds(200);
	}
}
#endif
