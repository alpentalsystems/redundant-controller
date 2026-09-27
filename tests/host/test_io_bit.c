#include "check.h"
#include "io_bit.h"

static struct io_bit_input good(void)
{
	struct io_bit_input in = {.loop_high = true, .loop_low = false, .led_read = 0x10U,
				  .led_applied = 0x10U, .accel_ready = true, .magn_ready = true,
				  .uart_errors = {0U, 0U}, .watchdog_reset = false};

	return in;
}

static void test_all_pass(void)
{
	struct io_bit_input in = good();

	CHECK(io_bit_eval(&in) == 0U);
}

static void test_loopback(void)
{
	struct io_bit_input in = good();

	in.loop_high = false; /* jumper removed */
	CHECK(io_bit_eval(&in) == IO_BIT_LOOPBACK);
	in = good();
	in.loop_low = true; /* stuck high */
	CHECK(io_bit_eval(&in) == IO_BIT_LOOPBACK);
}

static void test_led_readback(void)
{
	struct io_bit_input in = good();

	in.led_read = 0x00U;
	CHECK(io_bit_eval(&in) == IO_BIT_LEDS);
}

static void test_sensors(void)
{
	struct io_bit_input in = good();

	in.accel_ready = false;
	CHECK(io_bit_eval(&in) == IO_BIT_SENSORS);
	in = good();
	in.magn_ready = false;
	CHECK(io_bit_eval(&in) == IO_BIT_SENSORS);
}

static void test_uart_error_limit(void)
{
	struct io_bit_input in = good();

	in.uart_errors[0] = IO_BIT_UART_ERROR_LIMIT - 1U;
	CHECK(io_bit_eval(&in) == 0U);
	in.uart_errors[0] = IO_BIT_UART_ERROR_LIMIT;
	CHECK(io_bit_eval(&in) == IO_BIT_UART_A);
	in = good();
	in.uart_errors[1] = IO_BIT_UART_ERROR_LIMIT;
	CHECK(io_bit_eval(&in) == IO_BIT_UART_B);
}

static void test_watchdog_reset(void)
{
	struct io_bit_input in = good();

	in.watchdog_reset = true;
	CHECK(io_bit_eval(&in) == IO_BIT_RESET);
}

int main(void)
{
	test_all_pass();
	test_loopback();
	test_led_readback();
	test_sensors();
	test_uart_error_limit();
	test_watchdog_reset();
	return CHECK_DONE();
}
