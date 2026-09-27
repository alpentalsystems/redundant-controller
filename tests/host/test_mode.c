#include "check.h"
#include "mode.h"

/* Samples the button every 1 ms in [from, to); returns the number of toggles. */
static int hold(struct mode_state *m, bool pressed, int64_t from, int64_t to)
{
	int toggles = 0;

	for (int64_t t = from; t < to; t++) {
		if (mode_on_sample(m, pressed, t)) {
			toggles++;
		}
	}
	return toggles;
}

static void test_press_toggles_once(void)
{
	struct mode_state m;

	mode_init(&m, false, 0);
	CHECK(m.mode == RC_MODE_OPERATIONAL);
	CHECK(hold(&m, true, 0, 200) == 1);
	CHECK(m.mode == RC_MODE_TEST);
	CHECK(hold(&m, false, 200, 400) == 0);
	CHECK(hold(&m, true, 400, 600) == 1);
	CHECK(m.mode == RC_MODE_OPERATIONAL);
}

static void test_bounce_counts_once(void)
{
	struct mode_state m;
	int toggles = 0;

	mode_init(&m, false, 0);
	for (int64_t t = 0; t < 40; t++) {
		if (mode_on_sample(&m, ((t / 5) % 2) == 0, t)) {
			toggles++;
		}
	}
	toggles += hold(&m, true, 40, 200);
	CHECK(toggles == 1);
	CHECK(m.mode == RC_MODE_TEST);
}

static void test_short_glitch_ignored(void)
{
	struct mode_state m;

	mode_init(&m, false, 0);
	CHECK(hold(&m, true, 0, 30) == 0);
	CHECK(hold(&m, false, 30, 200) == 0);
	CHECK(m.mode == RC_MODE_OPERATIONAL);
}

static void test_held_at_boot_does_not_toggle(void)
{
	struct mode_state m;

	mode_init(&m, true, 0);
	CHECK(hold(&m, true, 0, 500) == 0);
	CHECK(m.mode == RC_MODE_OPERATIONAL);
}

int main(void)
{
	test_press_toggles_once();
	test_bounce_counts_once();
	test_short_glitch_ignored();
	test_held_at_boot_does_not_toggle();
	return CHECK_DONE();
}
