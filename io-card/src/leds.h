#ifndef LEDS_H_
#define LEDS_H_

#include <stdint.h>

int leds_init(void);

/* Bit i lights ring position i (0 = N, clockwise). */
int leds_set_mask(uint8_t mask);

/* Reads the ring back from the pins; returns the mask or a negative error. */
int leds_get_mask(void);

#endif /* LEDS_H_ */
