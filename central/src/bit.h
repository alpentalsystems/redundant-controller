#ifndef BIT_H_
#define BIT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BIT_IO_LINK 0
#define BIT_LOOP_TIMING 1
#define BIT_CROSS_LINK 2
#define BIT_SUPPLY 3
#define BIT_CPU_TEMP 4
#define BIT_ITEMS 5

#define BIT_STATUS_MAX_AGE_MS 100
#define BIT_LINK_ERROR_LIMIT 5U
#define BIT_MAX_HB_INTERVAL_MS 50
#define BIT_PEER_MAX_AGE_MS 100
#define BIT_TEMP_LIMIT_MC 80000
#define BIT_RECOVERY_PASSES 3U
#define BIT_PERIOD_MS 1000
#define BIT_PBIT_DELAY_MS 1000

enum bit_result { BIT_PASS, BIT_FAIL, BIT_ERROR };

struct bit_input {
	int64_t status_age_ms;
	uint32_t link_errors; /* CRC and length errors since the last check */
	int64_t max_hb_interval_ms;
	int64_t peer_age_ms;
	bool undervoltage_ok; /* false: the flag could not be read */
	bool undervoltage;
	bool temp_ok; /* false: the temperature could not be read */
	int32_t temp_mc;
};

struct bit_report {
	bool valid;
	int64_t at_ms;
	struct bit_input in;
	enum bit_result result[BIT_ITEMS];
};

struct bit_health {
	bool pbit_done;
	bool healthy;
	unsigned pass_run;
};

void bit_evaluate(const struct bit_input *in, int64_t now_ms, struct bit_report *out);
bool bit_is_critical(int item);
bool bit_critical_pass(const struct bit_report *r);
const char *bit_item_name(int item);
const char *bit_result_name(enum bit_result r);

/* Writes the measured value as text, e.g. "status_age_ms=12 errors=0". */
void bit_item_value(const struct bit_report *r, int item, char *out, size_t out_size);

void bit_health_init(struct bit_health *h);

/* Applies one check; returns true when healthy changed. The first check is PBIT. */
bool bit_health_update(struct bit_health *h, const struct bit_report *r);

#endif /* BIT_H_ */
