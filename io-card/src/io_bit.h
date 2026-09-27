#ifndef IO_BIT_H_
#define IO_BIT_H_

#include <stdbool.h>
#include <stdint.h>

#define IO_BIT_LOOPBACK (1U << 0)
#define IO_BIT_LEDS (1U << 1)
#define IO_BIT_SENSORS (1U << 2)
#define IO_BIT_UART_A (1U << 3)
#define IO_BIT_UART_B (1U << 4)
#define IO_BIT_RESET (1U << 5)

/* A slot fails at this many CRC or length errors in one check period. */
#define IO_BIT_UART_ERROR_LIMIT 5U

struct io_bit_input {
	bool loop_high; /* loopback input read while the output drives high */
	bool loop_low;  /* loopback input read while the output drives low */
	uint8_t led_read;
	uint8_t led_applied;
	bool accel_ready;
	bool magn_ready;
	uint32_t uart_errors[2]; /* errors since the last check, per slot */
	bool watchdog_reset;
};

/* Returns the fail mask (IO_BIT_* bits). */
uint8_t io_bit_eval(const struct io_bit_input *in);

#endif /* IO_BIT_H_ */
