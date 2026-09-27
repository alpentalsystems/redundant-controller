#include <stdio.h>

#include "bit.h"

static const char *const names[BIT_ITEMS] = {"io_link", "loop_timing", "cross_link",
					     "supply_voltage", "cpu_temp"};

static enum bit_result pass_if(bool ok)
{
	return ok ? BIT_PASS : BIT_FAIL;
}

void bit_evaluate(const struct bit_input *in, int64_t now_ms, struct bit_report *out)
{
	out->valid = true;
	out->at_ms = now_ms;
	out->in = *in;
	out->result[BIT_IO_LINK] = pass_if((in->status_age_ms <= BIT_STATUS_MAX_AGE_MS) &&
					   (in->link_errors < BIT_LINK_ERROR_LIMIT));
	out->result[BIT_LOOP_TIMING] = pass_if(in->max_hb_interval_ms <= BIT_MAX_HB_INTERVAL_MS);
	out->result[BIT_CROSS_LINK] = pass_if(in->peer_age_ms <= BIT_PEER_MAX_AGE_MS);
	out->result[BIT_SUPPLY] = in->undervoltage_ok ? pass_if(!in->undervoltage) : BIT_ERROR;
	out->result[BIT_CPU_TEMP] = in->temp_ok ? pass_if(in->temp_mc < BIT_TEMP_LIMIT_MC)
						: BIT_ERROR;
}

bool bit_is_critical(int item)
{
	return (item == BIT_IO_LINK) || (item == BIT_LOOP_TIMING);
}

bool bit_critical_pass(const struct bit_report *r)
{
	for (int i = 0; i < BIT_ITEMS; i++) {
		if (bit_is_critical(i) && (r->result[i] != BIT_PASS)) {
			return false;
		}
	}
	return true;
}

const char *bit_item_name(int item)
{
	return ((item >= 0) && (item < BIT_ITEMS)) ? names[item] : "unknown";
}

const char *bit_result_name(enum bit_result r)
{
	switch (r) {
	case BIT_PASS:
		return "pass";
	case BIT_FAIL:
		return "fail";
	case BIT_ERROR:
		return "error";
	}
	return "unknown";
}

void bit_item_value(const struct bit_report *r, int item, char *out, size_t out_size)
{
	const struct bit_input *in = &r->in;

	switch (item) {
	case BIT_IO_LINK:
		snprintf(out, out_size, "status_age_ms=%lld errors=%u", (long long)in->status_age_ms,
			 (unsigned)in->link_errors);
		break;
	case BIT_LOOP_TIMING:
		snprintf(out, out_size, "max_interval_ms=%lld", (long long)in->max_hb_interval_ms);
		break;
	case BIT_CROSS_LINK:
		snprintf(out, out_size, "peer_age_ms=%lld", (long long)in->peer_age_ms);
		break;
	case BIT_SUPPLY:
		if (in->undervoltage_ok) {
			snprintf(out, out_size, "undervoltage=%d", in->undervoltage ? 1 : 0);
		} else {
			snprintf(out, out_size, "read error");
		}
		break;
	case BIT_CPU_TEMP:
		if (in->temp_ok) {
			snprintf(out, out_size, "temp_mc=%ld", (long)in->temp_mc);
		} else {
			snprintf(out, out_size, "read error");
		}
		break;
	default:
		snprintf(out, out_size, "-");
		break;
	}
}

void bit_health_init(struct bit_health *h)
{
	h->pbit_done = false;
	h->healthy = false;
	h->pass_run = 0U;
}

bool bit_health_update(struct bit_health *h, const struct bit_report *r)
{
	bool before = h->healthy;
	bool ok = bit_critical_pass(r);

	if (!h->pbit_done) {
		h->pbit_done = true;
		h->healthy = ok;
		h->pass_run = ok ? BIT_RECOVERY_PASSES : 0U;
	} else if (!ok) {
		h->healthy = false;
		h->pass_run = 0U;
	} else {
		if (h->pass_run < BIT_RECOVERY_PASSES) {
			h->pass_run++;
		}
		if (h->pass_run >= BIT_RECOVERY_PASSES) {
			h->healthy = true;
		}
	}
	return h->healthy != before;
}
