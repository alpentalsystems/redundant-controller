#include "mode.h"

void mode_init(struct mode_state *m, bool pressed, int64_t now_ms)
{
	m->mode = RC_MODE_OPERATIONAL;
	m->stable = pressed;
	m->raw = pressed;
	m->raw_since_ms = now_ms;
}

bool mode_on_sample(struct mode_state *m, bool pressed, int64_t now_ms)
{
	if (pressed != m->raw) {
		m->raw = pressed;
		m->raw_since_ms = now_ms;
		return false;
	}
	if ((pressed == m->stable) || ((now_ms - m->raw_since_ms) < MODE_DEBOUNCE_MS)) {
		return false;
	}
	m->stable = pressed;
	if (!pressed) {
		return false;
	}
	m->mode = (m->mode == RC_MODE_TEST) ? RC_MODE_OPERATIONAL : RC_MODE_TEST;
	return true;
}
