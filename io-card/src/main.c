#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include "arbiter.h"
#include "io_bit.h"
#include "leds.h"
#include "mode.h"
#include "rc/proto.h"

LOG_MODULE_REGISTER(io_card, LOG_LEVEL_INF);

#define WDT_TIMEOUT_MS 250U
#define RX_BUF_SIZE 256U
#define IO_BIT_PERIOD_MS 1000
/* Loopback jumper: PD12 (output) to PD13 (input). */
#define LOOP_OUT_PIN 12
#define LOOP_IN_PIN 13

struct link {
	const struct device *dev;
	uint8_t slot;
	struct ring_buf rb;
	uint8_t rb_mem[RX_BUF_SIZE];
	struct rc_parser parser;
	uint32_t rx_dropped;
	uint32_t outputs_rejected;
	uint32_t errors_at_check;
};

static struct link links[2] = {
	{.dev = DEVICE_DT_GET(DT_NODELABEL(usart2)), .slot = RC_SLOT_A},
	{.dev = DEVICE_DT_GET(DT_NODELABEL(uart4)), .slot = RC_SLOT_B},
};

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static struct arbiter arb;
static uint8_t applied_mask;
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct device *const gpiod = DEVICE_DT_GET(DT_NODELABEL(gpiod));
static const struct device *const accel = DEVICE_DT_GET(DT_NODELABEL(lsm303agr_accel));
static const struct device *const magn = DEVICE_DT_GET(DT_NODELABEL(lsm303agr_magn));
static struct mode_state mode;
static uint8_t io_fail;
static bool watchdog_reset;
static bool bit_requested;
static uint16_t boot_id;

static char slot_name(uint8_t slot)
{
	return (slot == RC_SLOT_A) ? 'A' : ((slot == RC_SLOT_B) ? 'B' : '-');
}

static void uart_isr(const struct device *dev, void *user_data)
{
	struct link *l = user_data;

	if (uart_irq_update(dev) == 0) {
		return;
	}
	while (uart_irq_rx_ready(dev) != 0) {
		uint8_t buf[16];
		int n = uart_fifo_read(dev, buf, sizeof(buf));
		uint32_t put;

		if (n <= 0) {
			break;
		}
		put = ring_buf_put(&l->rb, buf, (uint32_t)n);
		l->rx_dropped += (uint32_t)n - put;
	}
}

static void send_frame(const struct device *dev, const uint8_t *buf, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		uart_poll_out(dev, buf[i]);
	}
}

static void set_outputs(uint8_t mask)
{
	int err;

	if (mask == applied_mask) {
		return;
	}
	err = leds_set_mask(mask);
	if (err != 0) {
		LOG_ERR("LED update failed: %d", err);
		return;
	}
	applied_mask = mask;
}

static void handle_frame(struct link *l, const struct rc_frame *f, int64_t now)
{
	struct rc_heartbeat hb;
	struct rc_set_outputs so;

	if (rc_decode_heartbeat(f, &hb) == 0) {
		struct rc_status st;
		uint8_t out[RC_FRAME_MAX];
		size_t n;

		if (boot_id == 0U) {
			/* No RNG on this chip: the cycle count at the first heartbeat differs per start. */
			uint32_t c = k_cycle_get_32();

			boot_id = (uint16_t)(c ^ (c >> 16));
			if (boot_id == 0U) {
				boot_id = 1U;
			}
			LOG_INF("t=%lld boot_id=0x%04x", now, boot_id);
		}

		arb_on_heartbeat(&arb, l->slot, hb.role, hb.healthy != 0U, now);
		st.seq = hb.seq;
		st.slot = l->slot;
		st.granted_role = arb_granted_role(&arb, l->slot);
		st.active_slot = arb_active(&arb);
		st.mode = mode.mode;
		st.io_fail = io_fail;
		st.io_time_ms = (uint32_t)now;
		st.io_boot_id = boot_id;
		n = rc_encode_status(&st, out, sizeof(out));
		send_frame(l->dev, out, n);
	} else if (rc_decode_set_outputs(f, &so) == 0) {
		if (arb_accepts_outputs(&arb, l->slot)) {
			set_outputs(so.mask);
		} else {
			l->outputs_rejected++;
		}
	} else if (rc_decode_run_bit(f) == 0) {
		if (arb_accepts_outputs(&arb, l->slot)) {
			bit_requested = true;
		} else {
			LOG_WRN("slot %c: RUN_BIT ignored, not Active", slot_name(l->slot));
		}
	} else {
		LOG_WRN("slot %c: unexpected frame type %u len %u", slot_name(l->slot), f->type,
			f->len);
	}
}

/* Drives the loopback output and reads the input: 1, 0, or a negative error. */
static int loop_read(int level)
{
	int err = gpio_pin_set(gpiod, LOOP_OUT_PIN, level);

	if (err != 0) {
		return err;
	}
	k_busy_wait(20);
	return gpio_pin_get(gpiod, LOOP_IN_PIN);
}

static void bit_run(int64_t now, bool pbit)
{
	struct io_bit_input in = {0};
	int led = leds_get_mask();
	uint8_t fail;

	in.loop_high = (loop_read(1) == 1);
	in.loop_low = (loop_read(0) != 0);
	in.led_read = (led < 0) ? (uint8_t)~applied_mask : (uint8_t)led;
	in.led_applied = applied_mask;
	in.accel_ready = device_is_ready(accel);
	in.magn_ready = device_is_ready(magn);
	for (size_t i = 0; i < ARRAY_SIZE(links); i++) {
		struct link *l = &links[i];
		uint32_t total = l->parser.crc_errors + l->parser.len_errors;

		in.uart_errors[i] = total - l->errors_at_check;
		l->errors_at_check = total;
	}
	in.watchdog_reset = watchdog_reset;
	fail = io_bit_eval(&in);
	if (pbit) {
		LOG_INF("t=%lld io_bit: pbit fail=0x%02x", now, fail);
	} else if (fail != io_fail) {
		LOG_INF("t=%lld io_bit: fail=0x%02x", now, fail);
	}
	io_fail = fail;
}

static int io_init(void)
{
	uint32_t cause;
	int err;

	if (!gpio_is_ready_dt(&button) || !device_is_ready(gpiod)) {
		return -ENODEV;
	}
	err = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (err == 0) {
		err = gpio_pin_configure(gpiod, LOOP_OUT_PIN, GPIO_OUTPUT_INACTIVE);
	}
	if (err == 0) {
		err = gpio_pin_configure(gpiod, LOOP_IN_PIN, GPIO_INPUT | GPIO_PULL_DOWN);
	}
	if (err != 0) {
		return err;
	}
	err = hwinfo_get_reset_cause(&cause);
	if (err != 0) {
		LOG_ERR("reset cause unavailable: %d", err);
	} else {
		watchdog_reset = (cause & RESET_WATCHDOG) != 0U;
		err = hwinfo_clear_reset_cause();
		if (err != 0) {
			LOG_ERR("reset cause clear failed: %d", err);
		}
	}
	mode_init(&mode, gpio_pin_get_dt(&button) == 1, k_uptime_get());
	return 0;
}

static int links_init(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(links); i++) {
		struct link *l = &links[i];
		int err;

		if (!device_is_ready(l->dev)) {
			return -ENODEV;
		}
		ring_buf_init(&l->rb, sizeof(l->rb_mem), l->rb_mem);
		rc_parser_init(&l->parser);
		err = uart_irq_callback_user_data_set(l->dev, uart_isr, l);
		if (err != 0) {
			return err;
		}
		uart_irq_rx_enable(l->dev);
	}
	return 0;
}

static int wdt_start(void)
{
	struct wdt_timeout_cfg cfg = {
		.window = {.min = 0U, .max = WDT_TIMEOUT_MS},
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};
	int ch;
	int err;

	if (!device_is_ready(wdt)) {
		return -ENODEV;
	}
	ch = wdt_install_timeout(wdt, &cfg);
	if (ch < 0) {
		return ch;
	}
	err = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (err != 0) {
		return err;
	}
	return ch;
}

int main(void)
{
	int64_t last_bit;
	bool button_error = false;
	int wdt_ch;
	int err;

	err = leds_init();
	if (err != 0) {
		LOG_ERR("LEDs not ready: %d", err);
		return 0;
	}
	err = links_init();
	if (err != 0) {
		LOG_ERR("UART links not ready: %d", err);
		return 0;
	}
	err = io_init();
	if (err != 0) {
		LOG_ERR("button or loopback pins not ready: %d", err);
		return 0;
	}
	wdt_ch = wdt_start();
	if (wdt_ch < 0) {
		LOG_ERR("watchdog start failed: %d", wdt_ch);
		return 0;
	}
	arb_init(&arb);
	LOG_INF("io-card ready: A=USART2 PA2/PA3, B=UART4 PC10/PC11, loopback PD12->PD13");
	last_bit = k_uptime_get();
	bit_run(last_bit, true);

	for (;;) {
		int64_t now = k_uptime_get();
		uint8_t before = arb_active(&arb);
		int b;

		for (size_t i = 0; i < ARRAY_SIZE(links); i++) {
			struct link *l = &links[i];
			uint8_t byte;

			while (ring_buf_get(&l->rb, &byte, 1U) == 1U) {
				struct rc_frame f;

				if (!rc_parser_feed(&l->parser, byte, &f)) {
					continue;
				}
				do {
					handle_frame(l, &f, now);
				} while (rc_parser_next(&l->parser, &f));
			}
		}
		if (arb_tick(&arb, now)) {
			uint8_t after = arb_active(&arb);
			int64_t gap = (before != RC_SLOT_NONE) ? now - arb_last_rx_ms(&arb, before)
							       : -1;

			LOG_INF("t=%lld active: %c -> %c gap_ms=%lld", now, slot_name(before),
				slot_name(after), gap);
		}
		if (arb_active(&arb) == RC_SLOT_NONE) {
			set_outputs(0U);
		}
		b = gpio_pin_get_dt(&button);
		if (b < 0) {
			if (!button_error) {
				LOG_ERR("button read failed: %d", b);
				button_error = true;
			}
		} else {
			button_error = false;
			if (mode_on_sample(&mode, b == 1, now)) {
				LOG_INF("t=%lld mode: %s", now,
					(mode.mode == RC_MODE_TEST) ? "test" : "operational");
			}
		}
		if (bit_requested || ((now - last_bit) >= IO_BIT_PERIOD_MS)) {
			bit_run(now, false);
			last_bit = now;
			bit_requested = false;
		}
		err = wdt_feed(wdt, wdt_ch);
		if (err != 0) {
			LOG_ERR("watchdog feed failed: %d", err);
		}
		k_msleep(1);
	}
	return 0;
}
