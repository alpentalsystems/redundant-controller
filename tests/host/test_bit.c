#include <string.h>

#include "bit.h"
#include "check.h"

static struct bit_input good(void)
{
	struct bit_input in = {.status_age_ms = 12, .link_errors = 0U, .max_hb_interval_ms = 21,
			       .peer_age_ms = 15, .undervoltage_ok = true, .undervoltage = false,
			       .temp_ok = true, .temp_mc = 48312};

	return in;
}

static enum bit_result item(const struct bit_input *in, int i)
{
	struct bit_report r;

	bit_evaluate(in, 1000, &r);
	return r.result[i];
}

static void test_all_pass(void)
{
	struct bit_input in = good();
	struct bit_report r;

	bit_evaluate(&in, 5000, &r);
	CHECK(r.valid && r.at_ms == 5000);
	for (int i = 0; i < BIT_ITEMS; i++) {
		CHECK(r.result[i] == BIT_PASS);
	}
	CHECK(bit_critical_pass(&r));
}

static void test_io_link_limits(void)
{
	struct bit_input in = good();

	in.status_age_ms = 100;
	CHECK(item(&in, BIT_IO_LINK) == BIT_PASS);
	in.status_age_ms = 101;
	CHECK(item(&in, BIT_IO_LINK) == BIT_FAIL);
	in = good();
	in.link_errors = 4U;
	CHECK(item(&in, BIT_IO_LINK) == BIT_PASS);
	in.link_errors = 5U;
	CHECK(item(&in, BIT_IO_LINK) == BIT_FAIL);
}

static void test_loop_timing_limit(void)
{
	struct bit_input in = good();

	in.max_hb_interval_ms = 50;
	CHECK(item(&in, BIT_LOOP_TIMING) == BIT_PASS);
	in.max_hb_interval_ms = 51;
	CHECK(item(&in, BIT_LOOP_TIMING) == BIT_FAIL);
}

static void test_cross_link_limit(void)
{
	struct bit_input in = good();

	in.peer_age_ms = 100;
	CHECK(item(&in, BIT_CROSS_LINK) == BIT_PASS);
	in.peer_age_ms = 101;
	CHECK(item(&in, BIT_CROSS_LINK) == BIT_FAIL);
}

static void test_supply_and_temp(void)
{
	struct bit_input in = good();

	in.undervoltage = true;
	CHECK(item(&in, BIT_SUPPLY) == BIT_FAIL);
	in.undervoltage_ok = false;
	CHECK(item(&in, BIT_SUPPLY) == BIT_ERROR);
	in = good();
	in.temp_mc = 79999;
	CHECK(item(&in, BIT_CPU_TEMP) == BIT_PASS);
	in.temp_mc = 80000;
	CHECK(item(&in, BIT_CPU_TEMP) == BIT_FAIL);
	in.temp_ok = false;
	CHECK(item(&in, BIT_CPU_TEMP) == BIT_ERROR);
}

static void test_only_link_and_timing_are_critical(void)
{
	struct bit_input in = good();
	struct bit_report r;

	CHECK(bit_is_critical(BIT_IO_LINK) && bit_is_critical(BIT_LOOP_TIMING));
	CHECK(!bit_is_critical(BIT_CROSS_LINK) && !bit_is_critical(BIT_SUPPLY));
	CHECK(!bit_is_critical(BIT_CPU_TEMP));
	in.peer_age_ms = 5000;
	in.undervoltage = true;
	in.temp_ok = false;
	bit_evaluate(&in, 0, &r);
	CHECK(bit_critical_pass(&r));
	in.status_age_ms = 500;
	bit_evaluate(&in, 0, &r);
	CHECK(!bit_critical_pass(&r));
}

static void test_names_and_values(void)
{
	struct bit_input in = good();
	struct bit_report r;
	char v[64];

	bit_evaluate(&in, 0, &r);
	CHECK(strcmp(bit_item_name(BIT_SUPPLY), "supply_voltage") == 0);
	CHECK(strcmp(bit_result_name(BIT_ERROR), "error") == 0);
	bit_item_value(&r, BIT_IO_LINK, v, sizeof(v));
	CHECK(strcmp(v, "status_age_ms=12 errors=0") == 0);
	bit_item_value(&r, BIT_CPU_TEMP, v, sizeof(v));
	CHECK(strcmp(v, "temp_mc=48312") == 0);
	in.undervoltage_ok = false;
	bit_evaluate(&in, 0, &r);
	bit_item_value(&r, BIT_SUPPLY, v, sizeof(v));
	CHECK(strcmp(v, "read error") == 0);
}

static struct bit_report report(bool critical_ok)
{
	struct bit_input in = good();
	struct bit_report r;

	if (!critical_ok) {
		in.status_age_ms = 1000;
	}
	bit_evaluate(&in, 0, &r);
	return r;
}

static void test_pbit_pass_is_healthy_at_once(void)
{
	struct bit_health h;
	struct bit_report ok = report(true);

	bit_health_init(&h);
	CHECK(!h.healthy && !h.pbit_done);
	CHECK(bit_health_update(&h, &ok));
	CHECK(h.healthy && h.pbit_done);
}

static void test_recovery_needs_three_passes(void)
{
	struct bit_health h;
	struct bit_report ok = report(true);
	struct bit_report bad = report(false);

	bit_health_init(&h);
	(void)bit_health_update(&h, &ok);
	CHECK(bit_health_update(&h, &bad));
	CHECK(!h.healthy);
	CHECK(!bit_health_update(&h, &ok));
	CHECK(!bit_health_update(&h, &ok));
	CHECK(bit_health_update(&h, &ok));
	CHECK(h.healthy);
}

static void test_pbit_fail_is_unhealthy(void)
{
	struct bit_health h;
	struct bit_report bad = report(false);
	struct bit_report ok = report(true);

	bit_health_init(&h);
	CHECK(!bit_health_update(&h, &bad));
	CHECK(!h.healthy && h.pbit_done);
	(void)bit_health_update(&h, &ok);
	(void)bit_health_update(&h, &ok);
	CHECK(!h.healthy);
	CHECK(bit_health_update(&h, &ok));
}

int main(void)
{
	test_all_pass();
	test_io_link_limits();
	test_loop_timing_limit();
	test_cross_link_limit();
	test_supply_and_temp();
	test_only_link_and_timing_are_critical();
	test_names_and_values();
	test_pbit_pass_is_healthy_at_once();
	test_recovery_needs_three_passes();
	test_pbit_fail_is_unhealthy();
	return CHECK_DONE();
}
