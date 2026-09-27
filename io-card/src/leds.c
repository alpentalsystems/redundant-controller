#include <errno.h>
#include <zephyr/drivers/gpio.h>

#include "leds.h"

static const struct gpio_dt_spec ring[8] = {
	GPIO_DT_SPEC_GET(DT_NODELABEL(red_led_3), gpios),    /* N  LD3  */
	GPIO_DT_SPEC_GET(DT_NODELABEL(orange_led_5), gpios), /* NE LD5  */
	GPIO_DT_SPEC_GET(DT_NODELABEL(green_led_7), gpios),  /* E  LD7  */
	GPIO_DT_SPEC_GET(DT_NODELABEL(blue_led_9), gpios),   /* SE LD9  */
	GPIO_DT_SPEC_GET(DT_NODELABEL(red_led_10), gpios),   /* S  LD10 */
	GPIO_DT_SPEC_GET(DT_NODELABEL(orange_led_8), gpios), /* SW LD8  */
	GPIO_DT_SPEC_GET(DT_NODELABEL(green_led_6), gpios),  /* W  LD6  */
	GPIO_DT_SPEC_GET(DT_NODELABEL(blue_led_4), gpios),   /* NW LD4  */
};

int leds_init(void)
{
	for (int i = 0; i < 8; i++) {
		int err;

		if (!gpio_is_ready_dt(&ring[i])) {
			return -ENODEV;
		}
		err = gpio_pin_configure_dt(&ring[i], GPIO_OUTPUT_INACTIVE);
		if (err != 0) {
			return err;
		}
	}
	return 0;
}

int leds_set_mask(uint8_t mask)
{
	for (int i = 0; i < 8; i++) {
		int err = gpio_pin_set_dt(&ring[i], (int)((mask >> i) & 1U));

		if (err != 0) {
			return err;
		}
	}
	return 0;
}

int leds_get_mask(void)
{
	int mask = 0;

	for (int i = 0; i < 8; i++) {
		int v = gpio_pin_get_dt(&ring[i]);

		if (v < 0) {
			return v;
		}
		mask |= (v & 1) << i;
	}
	return mask;
}
