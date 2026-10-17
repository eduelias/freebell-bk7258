/* SPDX-License-Identifier: Apache-2.0 */
/* freebell-bk7258: DMA audio engine: speaker/mic rings, tones and chimes, spk/mic commands, the aud_* API for talk.c. */
/* Amplifier on BOARD_PIN_AMP_EN (split out of app_main.c in 0.28.0) */
#include "app_internal.h"

#if (CONFIG_SYS_CPU0)
/* ---- audio: speaker tone / stock chimes from flash, mic level ---- */
#define AUDIO_RATE 8000
volatile bool s_spk_busy;   /* read by ringfx.c */
static volatile bool s_spk_stop;
static beken_thread_t s_spk_thread;
static uint32_t s_spk_hz, s_spk_ms, s_spk_addr, s_spk_len;
/* runtime audio config (spk cfg / mic cfg); PA = amplifier enable, SDK reference design uses GPIO 5 */
static int s_pa_pin = BOARD_PIN_AMP_EN;   /* speaker amplifier enable, active high (found by sweep 2026-10-03) */
static aud_dac_chl_t s_dac_chl = AUD_DAC_CHL_L;
static aud_dac_work_mode_t s_dac_mode = AUD_DAC_WORK_MODE_DIFFEN;
static int s_dac_gain_raw = -1;      /* `spk cfg gain N`: raw DAC gain override; -1 = follow `vol` (policy.c: 100 % = 0x1c, the loudest clean level
                                      * measured 2026-10-03; 0x2d = 0 dB clips the amplifier hard) */
static uint16_t spk_gain_now(void) { return s_dac_gain_raw >= 0 ? (uint16_t)s_dac_gain_raw : vol_gain(); }
static aud_adc_chl_t s_adc_chl = AUD_ADC_CHL_L;
static aud_adc_mode_t s_adc_mode = AUD_ADC_MODE_DIFFEN;
static aud_mic_id_t s_mic_id = AUD_MIC_MIC1;
static uint32_t s_adc_gain = 0x2d;
static uint32_t s_dac_rate = AUDIO_RATE, s_adc_rate = AUDIO_RATE;
static aud_clk_t s_dac_clk = AUD_CLK_XTAL, s_adc_clk = AUD_CLK_APLL;   /* SDK defaults; both measured off (DAC 3x, ADC 2x) */   /* configured rates; the hardware may run at another (measure!) */

/* ---- network audio (test rig): mic -> TCP as raw 32-bit FIFO words, TCP -> speaker as s16le 8 kHz mono.
 * The peer is a plain TCP listener (tools/audio_relay.py on a LAN host); the device always connects out. */
char s_net_host[32];   /* test rig peer, shared with `cam push` (camera.c) */
uint16_t s_net_port;
static uint16_t s_spk_net_port;
static uint32_t s_net_ms;
static volatile bool s_mic_busy;

int rig_tcp_connect(const char *host, uint16_t port)
{
	struct sockaddr_in sa = {0};
	struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
	int sock = socket(AF_INET, SOCK_STREAM, 0);

	if (sock < 0)
		return -1;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = inet_addr(host);
	if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		close(sock);
		return -1;
	}
	return sock;
}

void pa_set(int on)
{
	if (s_pa_pin < 0)
		return;
	pin_configure(s_pa_pin, GPIO_OUTPUT_ENABLE, GPIO_PULL_DISABLE);
	if (on)
		bk_gpio_set_output_high(s_pa_pin);
	else
		bk_gpio_set_output_low(s_pa_pin);
}

static const int16_t s_sine32[32] = {0,6393,12539,18204,23170,27245,30273,32137,32767,32137,30273,27245,23170,
	18204,12539,6393,0,-6393,-12539,-18204,-23170,-27245,-30273,-32137,-32767,-32137,-30273,-27245,-23170,-18204,-12539,-6393};

/* ---- DMA audio engine (v0.13.0): the DAC and ADC FIFOs are served by DMA in repeat mode over a
 * double buffer (2 x AUD_FRAME); the finish interrupt fires per half. Threads only touch the byte
 * rings s_spk_ring / s_mic_ring, so Wi-Fi preemption no longer glitches the audio. ---- */
#define AUD_FRAME 1024                 /* bytes per DMA half: 512 s16 samples = 64 ms at 8 kHz */
#define PA_SETTLE_MS 50                /* silence played before the PA is enabled / after it is disabled (pop fix) */
#define SPK_RING (16 * 1024)
#define MIC_RING (32 * 1024)
static uint8_t *s_dac_dma_buf, *s_adc_dma_buf;
static dma_id_t s_dac_dma = DMA_ID_MAX, s_adc_dma = DMA_ID_MAX;
static uint8_t s_spk_ring[SPK_RING], s_mic_ring[MIC_RING];
static volatile uint32_t s_spk_w, s_spk_r, s_mic_w, s_mic_r;   /* monotonic byte indices */
static volatile uint32_t s_dac_half, s_adc_half, s_spk_underrun, s_mic_overrun, s_adc_frames, s_dac_frames;
static volatile bool s_spk_feeding;

static void ring_copy_out(uint8_t *dst, const uint8_t *ring, uint32_t size, uint32_t idx, uint32_t n)
{
	uint32_t first = size - idx < n ? size - idx : n;
	os_memcpy(dst, ring + idx, first);
	if (n > first)
		os_memcpy(dst + first, ring, n - first);
}

static void ring_copy_in(uint8_t *ring, uint32_t size, uint32_t idx, const uint8_t *src, uint32_t n)
{
	uint32_t first = size - idx < n ? size - idx : n;
	os_memcpy(ring + idx, src, first);
	if (n > first)
		os_memcpy(ring, src + first, n - first);
}

static void dac_fill_half(uint32_t half)
{
	uint8_t *dst = s_dac_dma_buf + half * AUD_FRAME;
	uint32_t avail = s_spk_w - s_spk_r, n = avail < AUD_FRAME ? avail : AUD_FRAME;

	ring_copy_out(dst, s_spk_ring, SPK_RING, s_spk_r % SPK_RING, n);
	s_spk_r += n;
	if (n < AUD_FRAME) {
		os_memset(dst + n, 0, AUD_FRAME - n);
		if (s_spk_feeding)
			s_spk_underrun++;
	}
}

static void dac_dma_isr(dma_id_t id)
{
	dac_fill_half(s_dac_half);   /* the half the DMA just finished reading */
	s_dac_half ^= 1;
	s_dac_frames++;
}

static void adc_dma_isr(dma_id_t id)
{
	const uint8_t *src = s_adc_dma_buf + s_adc_half * AUD_FRAME;

	if (MIC_RING - (s_mic_w - s_mic_r) >= AUD_FRAME) {
		ring_copy_in(s_mic_ring, MIC_RING, s_mic_w % MIC_RING, src, AUD_FRAME);
		s_mic_w += AUD_FRAME;
	} else {
		s_mic_overrun++;
	}
	s_adc_half ^= 1;
	s_adc_frames++;
}

static void dma_port(dma_port_config_t *pc, dma_dev_t dev, dma_data_width_t w, uint32_t start, uint32_t end)
{
	pc->dev = dev;
	pc->width = w;
	pc->addr_inc_en = DMA_ADDR_INC_ENABLE;
	pc->addr_loop_en = DMA_ADDR_LOOP_ENABLE;
	pc->start_addr = start;
	pc->end_addr = end;
}

/* dir 0 = DAC (memory -> DAC FIFO, 16-bit mono writes), 1 = ADC (ADC FIFO -> memory, 32-bit words) */
static int aud_dma_start(int dir)
{
	dma_config_t c = {0};
	uint32_t fifo;
	uint8_t **buf = dir ? &s_adc_dma_buf : &s_dac_dma_buf;
	dma_id_t *id = dir ? &s_adc_dma : &s_dac_dma;

	if (!*buf)
		*buf = os_malloc(2 * AUD_FRAME);
	if (!*buf)
		return -1;
	os_memset(*buf, 0, 2 * AUD_FRAME);   /* the first two DAC halves play before the ISR refills them: silence, not heap garbage */
	bk_dma_driver_init();
	*id = bk_dma_alloc(DMA_DEV_AUDIO);
	if (*id >= DMA_ID_MAX)
		return -2;
	c.mode = DMA_WORK_MODE_REPEAT;
	c.chan_prio = 1;
	c.trans_type = DMA_TRANS_DEFAULT;
	if (dir) {
		bk_aud_adc_get_fifo_addr(&fifo);
		dma_port(&c.src, DMA_DEV_AUDIO_RX, DMA_DATA_WIDTH_32BITS, fifo, fifo + 4);
		dma_port(&c.dst, DMA_DEV_DTCM, DMA_DATA_WIDTH_32BITS, (uint32_t)*buf, (uint32_t)*buf + 2 * AUD_FRAME);
	} else {
		bk_aud_dac_get_fifo_addr(&fifo);
		dma_port(&c.src, DMA_DEV_DTCM, DMA_DATA_WIDTH_32BITS, (uint32_t)*buf, (uint32_t)*buf + 2 * AUD_FRAME);
		dma_port(&c.dst, DMA_DEV_AUDIO, DMA_DATA_WIDTH_16BITS, fifo, fifo + 2);
	}
	if (bk_dma_init(*id, &c) != BK_OK)
		return -3;
	bk_dma_set_transfer_len(*id, AUD_FRAME);
#if (CONFIG_SPE)
	bk_dma_set_dest_sec_attr(*id, DMA_ATTR_SEC);
	bk_dma_set_src_sec_attr(*id, DMA_ATTR_SEC);
#endif
	bk_dma_register_isr(*id, NULL, dir ? adc_dma_isr : dac_dma_isr);
	bk_dma_enable_finish_interrupt(*id);
	return bk_dma_start(*id) == BK_OK ? 0 : -4;
}

static void aud_dma_stop(int dir)
{
	dma_id_t *id = dir ? &s_adc_dma : &s_dac_dma;

	if (*id < DMA_ID_MAX) {
		bk_dma_stop(*id);
		bk_dma_disable_finish_interrupt(*id);
		bk_dma_deinit(*id);
		bk_dma_free(DMA_DEV_AUDIO, *id);
		*id = DMA_ID_MAX;
	}
}

/* producer side: append s16le bytes to the speaker ring, blocking while it is full */
static void spk_ring_write(const uint8_t *p, uint32_t n)
{
	while (n && !s_spk_stop) {
		uint32_t free = SPK_RING - (s_spk_w - s_spk_r), chunk;
		if (!free) {
			rtos_delay_milliseconds(5);
			continue;
		}
		chunk = n < free ? n : free;
		ring_copy_in(s_spk_ring, SPK_RING, s_spk_w % SPK_RING, p, chunk);
		s_spk_w += chunk;
		p += chunk;
		n -= chunk;
	}
}

/* While the voc engine (audio_voc.c) owns the hardware for an RTSP audio session, `spk` and the
 * chime play through its speaker tap instead of the raw DAC path, so a ring during a live call is
 * still heard at the door. */
static bool s_spk_voc;

static void spk_out(const uint8_t *p, uint32_t n)
{
	if (s_spk_voc) {
		if (voc_tap_write(p, n) != 0)
			s_spk_stop = true;
	} else {
		spk_ring_write(p, n);
	}
}

static void spk_task(beken_thread_arg_t arg)
{
	aud_dac_config_t cfg = DEFAULT_AUD_DAC_CONFIG();
	uint8_t buf[512];
	uint32_t i, n, phase = 0, step, total = 0;
	int rc;

	if (s_spk_voc) {
		if (voc_tap_open() != 0) {
			os_printf("[spk] voc tap busy (a /speak is playing): skipped\r\n");
			goto out;
		}
		goto feed;
	}
	cfg.samp_rate = s_dac_rate;
	cfg.clk_src = s_dac_clk;
	cfg.dac_chl = s_dac_chl;
	cfg.work_mode = s_dac_mode;
	cfg.dac_gain = spk_gain_now();
	s_spk_w = s_spk_r = 0;
	s_dac_half = 0;
	s_spk_underrun = s_dac_frames = 0;
	s_spk_feeding = false;   /* the pre-roll below plays zeros on purpose: don't count it as underruns */
	if (bk_aud_dac_init(&cfg) != BK_OK) {
		os_printf("[spk] dac init failed\r\n");
		goto out;
	}
	/* Amplifier pop fix (v0.19): start the DAC + DMA first (the ring is empty, so the DMA streams
	 * zeros = the DAC's idle level), let the analog output settle, and only then enable the PA. */
	if ((rc = aud_dma_start(0)) != 0 || bk_aud_dac_start() != BK_OK) {
		os_printf("[spk] dma/dac start failed (%d)\r\n", rc);
		aud_dma_stop(0);
		bk_aud_dac_deinit();
		pa_set(0);
		goto out;
	}
	rtos_delay_milliseconds(PA_SETTLE_MS);
	pa_set(1);
	rtos_delay_milliseconds(PA_SETTLE_MS);   /* the amplifier's own turn-on transient, on silence */
	s_spk_feeding = true;
feed:
	if (s_spk_net_port) {   /* s16le mono PCM streamed from a TCP peer */
		int sock = rig_tcp_connect(s_net_host, s_spk_net_port), r;
		if (sock < 0)
			os_printf("[spk] connect %s:%u failed\r\n", s_net_host, s_spk_net_port);
		while (sock >= 0 && !s_spk_stop && (r = recv(sock, buf, sizeof(buf), 0)) > 0) {
			spk_out(buf, r);
			total += r / 2;
		}
		if (sock >= 0)
			close(sock);
		s_spk_net_port = 0;
	} else if (s_spk_addr) {   /* s16le mono PCM from flash (stock chimes) */
		for (i = 0; i < s_spk_len && !s_spk_stop; i += n) {
			n = s_spk_len - i < sizeof(buf) ? s_spk_len - i : sizeof(buf);
			bk_flash_read_bytes(s_spk_addr + i, buf, n);
			spk_out(buf, n);
			total += n / 2;
		}
	} else {                   /* tone: 32-step sine, 12-bit phase accumulator */
		step = (s_spk_hz * 32u * 4096u) / s_dac_rate;
		for (i = 0; i < s_spk_ms * (s_dac_rate / 1000) && !s_spk_stop; i += n) {
			n = 0;
			while (n < sizeof(buf) / 2 && i + n < s_spk_ms * (s_dac_rate / 1000)) {
				int16_t v = s_sine32[(phase >> 12) & 31] / 4;
				buf[2 * n] = (uint8_t)v;
				buf[2 * n + 1] = (uint8_t)(v >> 8);
				phase += step;
				n++;
			}
			spk_out(buf, 2 * n);
			total += n;
		}
	}
	if (s_spk_voc) {
		voc_tap_close();
		os_printf("[spk] done via voc tap, %u samples\r\n", total);
		goto out;
	}
	s_spk_feeding = false;
	for (i = 0; i < 100 && s_spk_w != s_spk_r && !s_spk_stop; i++)   /* let the DMA drain the ring */
		rtos_delay_milliseconds(10);
	rtos_delay_milliseconds(2 * AUD_FRAME / 2 * 1000 / s_dac_rate);   /* plus the two halves in flight */
	/* pop fix: the DMA is now streaming zeros again; drop the PA on silence, then stop the DAC */
	pa_set(0);
	rtos_delay_milliseconds(PA_SETTLE_MS);
	aud_dma_stop(0);
	bk_aud_dac_stop();
	bk_aud_dac_deinit();
	os_printf("[spk] done, %u samples, %u dma frames, %u underruns\r\n", total, s_dac_frames, s_spk_underrun);
out:
	s_spk_voc = false;
	s_spk_busy = false;
	s_spk_thread = NULL;
	rtos_delete_thread(NULL);
}

void spk_start(uint32_t hz, uint32_t ms, uint32_t addr, uint32_t len)
{
	GLOBAL_INT_DECLARATION();

	GLOBAL_INT_DISABLE();
	if (s_spk_busy || (voc_busy() && !voc_active())) {   /* voc starting or stopping: nobody owns the DAC yet */
		GLOBAL_INT_RESTORE();
		REPLY("[spk] busy\r\n");
		return;
	}
	s_spk_busy = true;
	s_spk_voc = voc_active();   /* live RTSP audio session: play through the voc speaker tap */
	GLOBAL_INT_RESTORE();
	s_spk_hz = hz; s_spk_ms = ms; s_spk_addr = addr; s_spk_len = len; s_spk_stop = false;
	if (rtos_create_thread(&s_spk_thread, BEKEN_APPLICATION_PRIORITY, "spk", spk_task, 4096, NULL) != BK_OK) {
		s_spk_busy = false;
		REPLY("[spk] thread failed\r\n");
	}
}

void cli_spk_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	if (argc >= 2 && os_strcmp(argv[1], "tone") == 0) {
		uint32_t hz = argc >= 3 ? os_strtoul(argv[2], NULL, 10) : 1000;
		uint32_t ms = argc >= 4 ? os_strtoul(argv[3], NULL, 10) : 1000;
		REPLY("[spk] tone %u Hz %u ms\r\n", hz, ms);
		spk_start(hz, ms, 0, 0);
	} else if (argc >= 3 && os_strcmp(argv[1], "play") == 0) {
		/* stock Tuya chimes still in flash (resource FS at 0x600000): raw s16le 8 kHz mono */
		uint32_t addr = 0, len = 0;
		if (os_strcmp(argv[2], "beepa") == 0) { addr = 0x601000; len = 23180; }
		else if (os_strcmp(argv[2], "beepb") == 0) { addr = 0x607000; len = 26104; }
		else if (os_strcmp(argv[2], "bell") == 0) { addr = 0x60e000 + 16; len = 66450 - 16; }
		else if (argc >= 4) { addr = os_strtoul(argv[2], NULL, 16); len = os_strtoul(argv[3], NULL, 10); }
		if (!addr) { REPLY("spk play beepa|beepb|bell|HEXADDR LEN\r\n"); return; }
		REPLY("[spk] play 0x%x len %u\r\n", addr, len);
		spk_start(0, 0, addr, len);
	} else if (argc >= 4 && os_strcmp(argv[1], "net") == 0) {
		if (s_spk_busy) { REPLY("[spk] busy\r\n"); return; }
		os_strncpy(s_net_host, argv[2], sizeof(s_net_host) - 1);
		s_spk_net_port = os_strtoul(argv[3], NULL, 10);
		REPLY("[spk] stream from %s:%u\r\n", s_net_host, s_spk_net_port);
		spk_start(0, 0, 0, 0);
	} else if (argc >= 2 && os_strcmp(argv[1], "stop") == 0) {
		s_spk_stop = true;
		REPLY("[spk] stop requested\r\n");
	} else if (argc >= 3 && os_strcmp(argv[1], "pa") == 0) {
		s_pa_pin = os_strcmp(argv[2], "off") == 0 ? -1 : (int)os_strtoul(argv[2], NULL, 10);
		REPLY("[spk] pa pin %d\r\n", s_pa_pin);
	} else if (argc >= 2 && os_strcmp(argv[1], "cfg") == 0) {
		for (int i = 2; i + 1 < argc; i += 2) {
			if (!os_strcmp(argv[i], "chl"))
				s_dac_chl = !os_strcmp(argv[i + 1], "R") ? AUD_DAC_CHL_R : !os_strcmp(argv[i + 1], "LR") ? AUD_DAC_CHL_LR : AUD_DAC_CHL_L;
			else if (!os_strcmp(argv[i], "mode"))
				s_dac_mode = !os_strcmp(argv[i + 1], "se") ? AUD_DAC_WORK_MODE_SIGNAL_END : AUD_DAC_WORK_MODE_DIFFEN;
			else if (!os_strcmp(argv[i], "gain")) {
				s_dac_gain_raw = !os_strcmp(argv[i + 1], "auto") ? -1 : (int)os_strtoul(argv[i + 1], NULL, 0);   /* raw override; `auto` = back to `vol` */
				voc_set_spk_gain(spk_gain_now());   /* a live voc session follows it */
			} else if (!os_strcmp(argv[i], "rate"))
				s_dac_rate = os_strtoul(argv[i + 1], NULL, 10);
			else if (!os_strcmp(argv[i], "clk"))
				s_dac_clk = !os_strcmp(argv[i + 1], "apll") ? AUD_CLK_APLL : AUD_CLK_XTAL;
		}
		REPLY("[spk] cfg chl %d mode %d gain 0x%x%s rate %u clk %s pa %d\r\n", s_dac_chl, s_dac_mode, spk_gain_now(),
		      s_dac_gain_raw >= 0 ? " (raw override)" : " (vol)", s_dac_rate, s_dac_clk == AUD_CLK_APLL ? "apll" : "xtal", s_pa_pin);
	} else {
		REPLY("spk tone [HZ] [MS] | play beepa|beepb|bell|ADDR LEN | net HOST PORT | stop | pa PIN|off | cfg [chl L|R|LR] [mode diff|se] [gain N] [rate HZ] [clk xtal|apll]  (busy %d)\r\n", s_spk_busy);
	}
}

static void micnet_task(beken_thread_arg_t arg)
{
	aud_adc_config_t cfg = DEFAULT_AUD_ADC_CONFIG();
	uint32_t sent = 0, t0, t1 = 0, idx, n;
	int sock = rig_tcp_connect(s_net_host, s_net_port), rc;

	if (sock < 0) {
		os_printf("[mic] connect %s:%u failed\r\n", s_net_host, s_net_port);
		goto out;
	}
	cfg.samp_rate = s_adc_rate;
	cfg.clk_src = s_adc_clk;
	cfg.adc_chl = s_adc_chl;
	cfg.adc_mode = s_adc_mode;
	cfg.adc_gain = s_adc_gain;
	if (bk_aud_adc_init(&cfg) != BK_OK) {
		os_printf("[mic] adc init failed\r\n");
		close(sock);
		goto out;
	}
	bk_aud_adc_set_mic_mode(s_mic_id, s_adc_mode);
	s_mic_w = s_mic_r = 0;
	s_adc_half = 0;
	s_mic_overrun = s_adc_frames = 0;
	if ((rc = aud_dma_start(1)) != 0 || bk_aud_adc_start() != BK_OK) {
		os_printf("[mic] dma/adc start failed (%d)\r\n", rc);
		aud_dma_stop(1);
		bk_aud_adc_deinit();
		close(sock);
		goto out;
	}
	t0 = rtos_get_time();
	for (;;) {
		bool running = rtos_get_time() - t0 < s_net_ms;
		if (!running && s_adc_dma < DMA_ID_MAX) {   /* time is up: stop capturing, then drain */
			bk_aud_adc_stop();
			aud_dma_stop(1);
			t1 = rtos_get_time();
		}
		if (s_mic_w == s_mic_r) {
			if (!running)
				break;
			rtos_delay_milliseconds(5);
			continue;
		}
		idx = s_mic_r % MIC_RING;
		n = s_mic_w - s_mic_r;
		if (n > MIC_RING - idx)
			n = MIC_RING - idx;
		if (n > 4096)
			n = 4096;
		if (send(sock, &s_mic_ring[idx], n, 0) != (int)n)
			break;
		s_mic_r += n;
		sent += n;
	}
	if (s_adc_dma < DMA_ID_MAX) {
		bk_aud_adc_stop();
		aud_dma_stop(1);
		t1 = rtos_get_time();
	}
	bk_aud_adc_deinit();
	close(sock);
	os_printf("[mic] streamed %u bytes to %s:%u: %u dma frames in %u ms = %u words/s, %u overruns\r\n",
		  sent, s_net_host, s_net_port, s_adc_frames, t1 - t0,
		  (t1 - t0) ? (uint32_t)((uint64_t)s_adc_frames * AUD_FRAME / 4 * 1000 / (t1 - t0)) : 0, s_mic_overrun);
out:
	s_mic_busy = false;
	rtos_delete_thread(NULL);
}

void cli_mic_cmd(char *pcWriteBuffer, int xWriteBufferLen, int argc, char **argv)
{
	aud_adc_config_t cfg = DEFAULT_AUD_ADC_CONFIG();
	uint32_t ms = argc >= 3 ? os_strtoul(argv[2], NULL, 10) : 2000, st, w, n = 0, t0;
	uint32_t hz = argc >= 4 ? os_strtoul(argv[3], NULL, 10) : 1000;
	/* the FIFO word carries two 16-bit halves (L low, R high); report both, we don't know which
	 * input the board's electret sits on */
	struct { int32_t peak; uint64_t sq; int64_t sum; float g1, g2; } ch[2] = {{0}};
	/* Goertzel at hz: how much of the signal is a tone (a Mac playing hz into the mic) vs noise */
	float gc = 2.0f * cosf(2.0f * 3.14159265f * (float)hz / (float)AUDIO_RATE), g0;

	if (argc >= 2 && os_strcmp(argv[1], "cfg") == 0) {
		for (int i = 2; i + 1 < argc; i += 2) {
			if (!os_strcmp(argv[i], "mic"))
				s_mic_id = !os_strcmp(argv[i + 1], "2") ? AUD_MIC_MIC2 : !os_strcmp(argv[i + 1], "both") ? AUD_MIC_BOTH : AUD_MIC_MIC1;
			else if (!os_strcmp(argv[i], "mode"))
				s_adc_mode = !os_strcmp(argv[i + 1], "se") ? AUD_ADC_MODE_SIGNAL_END : AUD_ADC_MODE_DIFFEN;
			else if (!os_strcmp(argv[i], "gain"))
				s_adc_gain = os_strtoul(argv[i + 1], NULL, 0);
			else if (!os_strcmp(argv[i], "chl"))
				s_adc_chl = !os_strcmp(argv[i + 1], "R") ? AUD_ADC_CHL_R : !os_strcmp(argv[i + 1], "LR") ? AUD_ADC_CHL_LR : AUD_ADC_CHL_L;
			else if (!os_strcmp(argv[i], "rate"))
				s_adc_rate = os_strtoul(argv[i + 1], NULL, 10);
			else if (!os_strcmp(argv[i], "clk"))
				s_adc_clk = !os_strcmp(argv[i + 1], "apll") ? AUD_CLK_APLL : AUD_CLK_XTAL;
		}
		REPLY("[mic] cfg mic %d mode %d gain 0x%x chl %d rate %u clk %s\r\n", s_mic_id, s_adc_mode, s_adc_gain, s_adc_chl, s_adc_rate, s_adc_clk == AUD_CLK_APLL ? "apll" : "xtal");
		return;
	}
	if (argc >= 5 && os_strcmp(argv[1], "rec") == 0) {
		beken_thread_t t;
		if (s_mic_busy || voc_busy()) { REPLY("[mic] busy%s\r\n", voc_busy() ? " (voc engine active: rtsp audio / talk voc)" : ""); return; }
		s_net_ms = os_strtoul(argv[2], NULL, 10);
		if (s_net_ms > 60000) s_net_ms = 60000;
		os_strncpy(s_net_host, argv[3], sizeof(s_net_host) - 1);
		s_net_port = os_strtoul(argv[4], NULL, 10);
		s_mic_busy = true;
		REPLY("[mic] rec %u ms -> %s:%u (raw 32-bit FIFO words, lo16 = L, hi16 = R)\r\n", s_net_ms, s_net_host, s_net_port);
		if (rtos_create_thread(&t, BEKEN_APPLICATION_PRIORITY, "micnet", micnet_task, 4096, NULL) != BK_OK) {
			s_mic_busy = false;
			REPLY("[mic] thread failed\r\n");
		}
		return;
	}
	if (argc < 2 || os_strcmp(argv[1], "level") != 0) {
		REPLY("mic rec MS HOST PORT | mic level [MS] [HZ] | cfg [mic 1|2|both] [mode diff|se] [gain N] [chl L|R|LR] [rate HZ] [clk xtal|apll]\r\n");
		return;
	}
	if (s_mic_busy || voc_busy()) {
		REPLY("[mic] busy%s\r\n", voc_busy() ? " (voc engine active)" : "");
		return;
	}
	if (ms > 4000)
		ms = 4000;
	cfg.samp_rate = s_adc_rate;
	cfg.clk_src = s_adc_clk;
	cfg.adc_chl = s_adc_chl;
	cfg.adc_mode = s_adc_mode;
	cfg.adc_gain = s_adc_gain;
	if (bk_aud_adc_init(&cfg) != BK_OK) {
		REPLY("[mic] adc init failed\r\n");
		return;
	}
	bk_aud_adc_set_mic_mode(s_mic_id, s_adc_mode);
	if (bk_aud_adc_start() != BK_OK) {
		REPLY("[mic] adc start failed\r\n");
		bk_aud_adc_deinit();
		return;
	}
	t0 = rtos_get_time();
	while (rtos_get_time() - t0 < ms) {
		bk_aud_adc_get_status(&st);
		if (st & AUD_ADCL_FIFO_EMPTY_MASK) {
			rtos_delay_milliseconds(1);
			continue;
		}
		bk_aud_adc_get_fifo_data(&w);
		n++;
		for (int c = 0; c < 2; c++) {
			int16_t v = (int16_t)(c ? (w >> 16) : (w & 0xffff));
			ch[c].sum += v;
			ch[c].sq += (int64_t)v * v;
			if (v > ch[c].peak) ch[c].peak = v;
			if (-v > ch[c].peak) ch[c].peak = -v;
			g0 = (float)v + gc * ch[c].g1 - ch[c].g2;
			ch[c].g2 = ch[c].g1;
			ch[c].g1 = g0;
		}
	}
	bk_aud_adc_stop();
	bk_aud_adc_deinit();
	if (n) {
		REPLY("[mic] %u samples in %u ms", n, ms);
		for (int c = 0; c < 2; c++) {
			int32_t mean = (int32_t)(ch[c].sum / (int64_t)n);
			uint32_t rms = 0, x = (uint32_t)(ch[c].sq / n);
			for (uint32_t b = 1u << 15; b; b >>= 1)   /* integer sqrt */
				if ((rms + b) * (rms + b) <= x) rms += b;
			float gm = sqrtf(ch[c].g1 * ch[c].g1 + ch[c].g2 * ch[c].g2 - gc * ch[c].g1 * ch[c].g2) / (float)n;
			REPLY(" | %s: peak %d rms %u dc %d tone%u %d", c ? "hi16" : "lo16", ch[c].peak, rms, mean, hz, (int)gm);
		}
		REPLY("\r\n");
	} else {
		REPLY("[mic] no samples (FIFO never filled)\r\n");
	}
}

/* ---- engine API for talk.c (aud_engine.h): the same DAC/ADC + DMA rings as `spk`/`mic`, but opened
 * and closed by the caller's thread (the HTTP client thread) instead of spk_task / micnet_task.
 * Mutual exclusion is the existing s_spk_busy / s_mic_busy, so the ring chime, `spk tone`, `mic rec`
 * and talk never drive the hardware at the same time. ---- */
/* While the voc engine runs (RTSP audio session / `talk voc on`), the talk.c users go through its
 * taps: /speak -> voc speaker tap (A-law encoded, AEC reference included), /mic.wav -> voc mic tap
 * (decoded, 8 kHz). s_spk_busy / s_mic_busy are set either way, so the chime, `spk` and `mic` see
 * the same exclusion as before. */
static bool s_talk_spk_voc, s_talk_mic_voc;

bool aud_raw_busy(void) { return s_spk_busy || s_mic_busy; }
void aud_pa_set(int on) { pa_set(on); }

int aud_spk_open(uint16_t gain)
{
	aud_dac_config_t cfg = DEFAULT_AUD_DAC_CONFIG();
	int rc;
	GLOBAL_INT_DECLARATION();

	GLOBAL_INT_DISABLE();
	if (s_spk_busy || (voc_busy() && !voc_active())) {
		GLOBAL_INT_RESTORE();
		return -1;
	}
	s_spk_busy = true;
	s_talk_spk_voc = voc_active();
	GLOBAL_INT_RESTORE();
	s_spk_stop = false;
	if (s_talk_spk_voc) {
		if (voc_tap_open() != 0) {
			s_spk_busy = false;
			return -1;
		}
		if (gain)
			voc_set_spk_gain(gain);
		return 0;
	}
	cfg.samp_rate = s_dac_rate;
	cfg.clk_src = s_dac_clk;
	cfg.dac_chl = s_dac_chl;
	cfg.work_mode = s_dac_mode;
	cfg.dac_gain = gain ? gain : spk_gain_now();
	s_spk_w = s_spk_r = 0;
	s_dac_half = 0;
	s_spk_underrun = s_dac_frames = 0;
	s_spk_feeding = true;
	if (bk_aud_dac_init(&cfg) != BK_OK) {
		s_spk_busy = false;
		return -2;
	}
	if (!s_dac_dma_buf)
		s_dac_dma_buf = os_malloc(2 * AUD_FRAME);
	if (s_dac_dma_buf)
		os_memset(s_dac_dma_buf, 0, 2 * AUD_FRAME);   /* no stale audio from the last playback before the PA comes on */
	if ((rc = aud_dma_start(0)) != 0 || bk_aud_dac_start() != BK_OK) {
		os_printf("[talk] dma/dac start failed (%d)\r\n", rc);
		aud_dma_stop(0);
		bk_aud_dac_deinit();
		s_spk_busy = false;
		return -3;
	}
	rtos_delay_milliseconds(20);   /* DAC runs on silence first, then the amplifier: softer pop */
	pa_set(1);
	return 0;
}

int aud_spk_write(const uint8_t *s16le, uint32_t bytes)
{
	if (s_talk_spk_voc)
		return voc_tap_write(s16le, bytes) != 0 || s_spk_stop ? -1 : 0;
	spk_ring_write(s16le, bytes);
	return s_spk_stop ? -1 : 0;
}

void aud_spk_close(void)
{
	uint32_t i;

	if (s_talk_spk_voc) {   /* the flag stays set until the next open so aud_spk_stats() still reports the tap */
		voc_tap_close();
		voc_set_spk_gain(spk_gain_now());   /* back to the session's gain (vol / raw override) */
		s_spk_busy = false;
		return;
	}
	s_spk_feeding = false;
	for (i = 0; i < 250 && s_spk_w != s_spk_r && !s_spk_stop; i++)   /* drain the ring (<= 1 s of audio) */
		rtos_delay_milliseconds(10);
	rtos_delay_milliseconds(2 * AUD_FRAME / 2 * 1000 / s_dac_rate);   /* plus the two DMA halves in flight */
	pa_set(0);                     /* amplifier off before the DAC stops: no stop transient on the speaker */
	rtos_delay_milliseconds(20);
	aud_dma_stop(0);
	bk_aud_dac_stop();
	bk_aud_dac_deinit();
	s_spk_busy = false;
}

bool aud_spk_busy(void) { return s_spk_busy; }
uint32_t aud_spk_rate(void) { return s_dac_rate; }
uint16_t aud_spk_gain(void) { return spk_gain_now(); }
void aud_spk_stats(uint32_t *dma_frames, uint32_t *underruns)
{
	if (s_talk_spk_voc) {
		voc_tap_stats(dma_frames, underruns);
		return;
	}
	*dma_frames = s_dac_frames;
	*underruns = s_spk_underrun;
}

int aud_mic_open(uint32_t gain)
{
	aud_adc_config_t cfg = DEFAULT_AUD_ADC_CONFIG();
	int rc;
	GLOBAL_INT_DECLARATION();

	GLOBAL_INT_DISABLE();
	if (s_mic_busy || (voc_busy() && !voc_active())) {
		GLOBAL_INT_RESTORE();
		return -1;
	}
	s_mic_busy = true;
	s_talk_mic_voc = voc_active();
	GLOBAL_INT_RESTORE();
	if (s_talk_mic_voc) {
		if (voc_mic_tap_open() != 0) {
			s_mic_busy = false;
			return -1;
		}
		return 0;
	}
	cfg.samp_rate = s_adc_rate;
	cfg.clk_src = s_adc_clk;
	cfg.adc_chl = s_adc_chl;
	cfg.adc_mode = s_adc_mode;
	cfg.adc_gain = gain ? gain : s_adc_gain;
	if (bk_aud_adc_init(&cfg) != BK_OK) {
		s_mic_busy = false;
		return -2;
	}
	bk_aud_adc_set_mic_mode(s_mic_id, s_adc_mode);
	s_mic_w = s_mic_r = 0;
	s_adc_half = 0;
	s_mic_overrun = s_adc_frames = 0;
	if ((rc = aud_dma_start(1)) != 0 || bk_aud_adc_start() != BK_OK) {
		os_printf("[talk] dma/adc start failed (%d)\r\n", rc);
		aud_dma_stop(1);
		bk_aud_adc_deinit();
		s_mic_busy = false;
		return -3;
	}
	return 0;
}

uint32_t aud_mic_read(uint8_t *dst, uint32_t cap)
{
	uint32_t n = s_mic_w - s_mic_r;

	if (s_talk_mic_voc)
		return voc_mic_tap_read(dst, cap);
	if (n > cap)
		n = cap;
	n &= ~3u;   /* whole 32-bit FIFO words */
	if (!n)
		return 0;
	ring_copy_out(dst, s_mic_ring, MIC_RING, s_mic_r % MIC_RING, n);
	s_mic_r += n;
	return n;
}

void aud_mic_close(void)
{
	if (s_talk_mic_voc) {
		voc_mic_tap_close();
		s_talk_mic_voc = false;
		s_mic_busy = false;
		return;
	}
	bk_aud_adc_stop();
	aud_dma_stop(1);
	bk_aud_adc_deinit();
	s_mic_busy = false;
}

bool aud_mic_busy(void) { return s_mic_busy; }
/* raw: measured 2026-10-03, the ADC delivers twice the configured rate; voc tap: decoded 8 kHz */
uint32_t aud_mic_rate(void) { return s_talk_mic_voc || (!s_mic_busy && voc_active()) ? VOC_RATE : 2 * s_adc_rate; }
void aud_mic_stats(uint32_t *dma_frames, uint32_t *overruns)
{
	if (s_talk_mic_voc) {
		voc_mic_tap_stats(dma_frames, overruns);
		return;
	}
	*dma_frames = s_adc_frames;
	*overruns = s_mic_overrun;
}
#endif

#if (CONFIG_SYS_CPU0)
/* power.c: the speaker or the microphone is in use */
bool audio_busy(void)
{
	return s_spk_busy || s_mic_busy;
}
#endif
