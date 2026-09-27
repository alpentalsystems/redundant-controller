#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include "arbiter.h"
#include "leds.h"
#include "rc/proto.h"

LOG_MODULE_REGISTER(io_card, LOG_LEVEL_INF);

#define WDT_TIMEOUT_MS 250U
#define RX_BUF_SIZE 256U

struct link {
	const struct device *dev;
	uint8_t slot;
	struct ring_buf rb;
	uint8_t rb_mem[RX_BUF_SIZE];
	struct rc_parser parser;
	uint32_t rx_dropped;
	uint32_t outputs_rejected;
};

static struct link links[2] = {
	{.dev = DEVICE_DT_GET(DT_NODELABEL(usart2)), .slot = RC_SLOT_A},
	{.dev = DEVICE_DT_GET(DT_NODELABEL(uart4)), .slot = RC_SLOT_B},
};

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static struct arbiter arb;
static uint8_t applied_mask;

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

		arb_on_heartbeat(&arb, l->slot, hb.role, now);
		st.seq = hb.seq;
		st.slot = l->slot;
		st.granted_role = arb_granted_role(&arb, l->slot);
		st.active_slot = arb_active(&arb);
		st.mode = RC_MODE_OPERATIONAL;
		st.io_fail = 0U;
		n = rc_encode_status(&st, out, sizeof(out));
		send_frame(l->dev, out, n);
	} else if (rc_decode_set_outputs(f, &so) == 0) {
		if (arb_accepts_outputs(&arb, l->slot)) {
			set_outputs(so.mask);
		} else {
			l->outputs_rejected++;
		}
	} else {
		LOG_WRN("slot %c: unexpected frame type %u len %u", slot_name(l->slot), f->type,
			f->len);
	}
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
	wdt_ch = wdt_start();
	if (wdt_ch < 0) {
		LOG_ERR("watchdog start failed: %d", wdt_ch);
		return 0;
	}
	arb_init(&arb);
	LOG_INF("io-card ready: A=USART2 PA2/PA3, B=UART4 PC10/PC11");

	for (;;) {
		int64_t now = k_uptime_get();
		uint8_t before = arb_active(&arb);

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
		err = wdt_feed(wdt, wdt_ch);
		if (err != 0) {
			LOG_ERR("watchdog feed failed: %d", err);
		}
		k_msleep(1);
	}
	return 0;
}
