#include <string.h>

#include "check.h"
#include "cmd.h"
#include "rc/proto.h"

static char out[CMD_REPLY_MAX];

static struct cmd_view view(uint8_t role, uint8_t mode)
{
	struct cmd_view v = {.slot = RC_SLOT_A, .role = role, .mode = mode,
			     .active_slot = (role == RC_ROLE_ACTIVE) ? RC_SLOT_A : RC_SLOT_B,
			     .healthy = true, .referee_ok = true, .peer_ok = true,
			     .peer_healthy = true, .fault = false, .step = 123U, .mask = 0x55U,
			     .io_fail = 0U, .pbit = NULL, .cbit = NULL, .now_ms = 5000};

	return v;
}

static const char *run(const char *line, const struct cmd_view *v, struct cmd_result *res)
{
	size_t n = cmd_handle(line, v, res, out, sizeof(out));

	CHECK(n == strlen(out));
	CHECK((n > 0U) && (out[n - 1U] == '\n'));
	return out;
}

static void test_status(void)
{
	struct cmd_view v = view(RC_ROLE_ACTIVE, RC_MODE_TEST);
	struct cmd_result res;

	run("STATUS", &v, &res);
	CHECK(res.action == CMD_ACT_NONE);
	CHECK(strstr(out, "\"ok\":true") != NULL);
	CHECK(strstr(out, "\"role\":\"active\"") != NULL);
	CHECK(strstr(out, "\"mode\":\"test\"") != NULL);
	CHECK(strstr(out, "\"step\":123") != NULL);
	CHECK(strstr(out, "\"mask\":85") != NULL);
	v = view(RC_ROLE_STANDBY, RC_MODE_OPERATIONAL);
	run("  STATUS \r", &v, &res);
	CHECK(strstr(out, "\"role\":\"standby\"") != NULL);
	CHECK(strstr(out, "\"mode\":\"operational\"") != NULL);
}

static void test_leds_in_test_mode(void)
{
	struct cmd_view v = view(RC_ROLE_ACTIVE, RC_MODE_TEST);
	struct cmd_result res;

	CHECK(strcmp(run("LEDS 0x55", &v, &res), "{\"ok\":true}\n") == 0);
	CHECK(res.action == CMD_ACT_SET_LEDS && res.leds == 0x55U);
	run("LEDS ff", &v, &res);
	CHECK(res.action == CMD_ACT_SET_LEDS && res.leds == 0xFFU);
}

static void test_leds_bad_values(void)
{
	static const char *const bad[] = {"LEDS 0x1FF", "LEDS zz", "LEDS", "LEDS -1", "LEDS 0x"};
	struct cmd_view v = view(RC_ROLE_ACTIVE, RC_MODE_TEST);
	struct cmd_result res;

	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		CHECK(strcmp(run(bad[i], &v, &res), "{\"ok\":false,\"error\":\"bad value\"}\n") == 0);
		CHECK(res.action == CMD_ACT_NONE);
	}
}

static void test_output_commands_refused(void)
{
	struct cmd_view op = view(RC_ROLE_ACTIVE, RC_MODE_OPERATIONAL);
	struct cmd_view sb = view(RC_ROLE_STANDBY, RC_MODE_TEST);
	struct cmd_result res;

	CHECK(strcmp(run("LEDS 0x55", &op, &res),
		     "{\"ok\":false,\"error\":\"operational mode\"}\n") == 0);
	CHECK(res.action == CMD_ACT_NONE);
	CHECK(strcmp(run("LAMP_TEST", &op, &res),
		     "{\"ok\":false,\"error\":\"operational mode\"}\n") == 0);
	CHECK(strcmp(run("RUN_BIT", &sb, &res),
		     "{\"ok\":false,\"error\":\"not active\",\"active\":\"B\"}\n") == 0);
	CHECK(res.action == CMD_ACT_NONE);
}

static void test_lamp_and_run_bit_actions(void)
{
	struct cmd_view v = view(RC_ROLE_ACTIVE, RC_MODE_TEST);
	struct cmd_result res;

	run("LAMP_TEST", &v, &res);
	CHECK(res.action == CMD_ACT_LAMP_TEST);
	run("RUN_BIT", &v, &res);
	CHECK(res.action == CMD_ACT_RUN_BIT);
}

static void test_unknown_commands(void)
{
	static const char *const unknown[] = {"HELLO", "", "status", "STATUS now", "LAMP_TEST 1"};
	struct cmd_view v = view(RC_ROLE_ACTIVE, RC_MODE_TEST);
	struct cmd_result res;

	for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
		CHECK(strcmp(run(unknown[i], &v, &res),
			     "{\"ok\":false,\"error\":\"unknown command\"}\n") == 0);
		CHECK(res.action == CMD_ACT_NONE);
	}
}

static void test_bit_reply(void)
{
	struct cmd_view v = view(RC_ROLE_ACTIVE, RC_MODE_OPERATIONAL);
	struct bit_input in = {.status_age_ms = INT64_MAX, .link_errors = UINT32_MAX,
			       .max_hb_interval_ms = INT64_MAX, .peer_age_ms = INT64_MAX,
			       .undervoltage_ok = true, .undervoltage = true,
			       .temp_ok = true, .temp_mc = INT32_MAX};
	struct bit_report r;
	struct cmd_result res;

	run("BIT", &v, &res);
	CHECK(strstr(out, "\"pbit\":null,\"cbit\":null") != NULL);
	bit_evaluate(&in, 4000, &r);
	v.pbit = &r;
	v.cbit = &r;
	run("BIT", &v, &res);
	CHECK(strstr(out, "\"age_ms\":1000") != NULL);
	CHECK(strstr(out, "{\"name\":\"io_link\",\"result\":\"fail\",\"critical\":true,") != NULL);
	CHECK(strstr(out, "\"name\":\"supply_voltage\",\"result\":\"fail\",\"critical\":false") !=
	      NULL);
	CHECK(strstr(out, ",\"io_fail\":0}\n") != NULL); /* not truncated */
}

static void test_linebuf(void)
{
	struct cmd_linebuf lb;
	char line[CMD_LINE_MAX + 1U];
	const char *in = "STATUS\r\n";

	cmd_linebuf_init(&lb);
	for (size_t i = 0; i < strlen(in) - 1U; i++) {
		CHECK(cmd_linebuf_feed(&lb, in[i], line) == CMD_FEED_NONE);
	}
	CHECK(cmd_linebuf_feed(&lb, '\n', line) == CMD_FEED_LINE);
	CHECK(strcmp(line, "STATUS") == 0);
	for (size_t i = 0; i < CMD_LINE_MAX; i++) {
		CHECK(cmd_linebuf_feed(&lb, 'x', line) == CMD_FEED_NONE);
	}
	CHECK(cmd_linebuf_feed(&lb, '\n', line) == CMD_FEED_LINE);
	CHECK(strlen(line) == CMD_LINE_MAX);
	for (size_t i = 0; i < CMD_LINE_MAX; i++) {
		(void)cmd_linebuf_feed(&lb, 'x', line);
	}
	CHECK(cmd_linebuf_feed(&lb, 'x', line) == CMD_FEED_TOO_LONG);
	CHECK(cmd_too_long(out, sizeof(out)) == strlen(out));
	CHECK(strcmp(out, "{\"ok\":false,\"error\":\"line too long\"}\n") == 0);
}

int main(void)
{
	test_status();
	test_leds_in_test_mode();
	test_leds_bad_values();
	test_output_commands_refused();
	test_lamp_and_run_bit_actions();
	test_unknown_commands();
	test_bit_reply();
	test_linebuf();
	return CHECK_DONE();
}
