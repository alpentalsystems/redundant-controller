#ifndef MODE_H_
#define MODE_H_

#include <stdbool.h>
#include <stdint.h>

#include "rc/proto.h"

#define MODE_DEBOUNCE_MS 50

struct mode_state {
	uint8_t mode;
	bool stable;
	bool raw;
	int64_t raw_since_ms;
};

void mode_init(struct mode_state *m, bool pressed, int64_t now_ms);

/* Feeds one button sample; returns true when a debounced press toggled the mode. */
bool mode_on_sample(struct mode_state *m, bool pressed, int64_t now_ms);

#endif /* MODE_H_ */
