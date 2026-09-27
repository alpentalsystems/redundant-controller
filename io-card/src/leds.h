#ifndef LEDS_H_
#define LEDS_H_

#include <stdint.h>

int leds_init(void);

/* Bit i lights ring position i (0 = N, clockwise). */
int leds_set_mask(uint8_t mask);

#endif /* LEDS_H_ */
