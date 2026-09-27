#include "io_bit.h"

uint8_t io_bit_eval(const struct io_bit_input *in)
{
	uint8_t fail = 0U;

	if (!in->loop_high || in->loop_low) {
		fail |= IO_BIT_LOOPBACK;
	}
	if (in->led_read != in->led_applied) {
		fail |= IO_BIT_LEDS;
	}
	if (!in->accel_ready || !in->magn_ready) {
		fail |= IO_BIT_SENSORS;
	}
	if (in->uart_errors[0] >= IO_BIT_UART_ERROR_LIMIT) {
		fail |= IO_BIT_UART_A;
	}
	if (in->uart_errors[1] >= IO_BIT_UART_ERROR_LIMIT) {
		fail |= IO_BIT_UART_B;
	}
	if (in->watchdog_reset) {
		fail |= IO_BIT_RESET;
	}
	return fail;
}
