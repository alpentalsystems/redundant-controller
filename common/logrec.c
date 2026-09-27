#include <string.h>

#include "rc/logrec.h"
#include "rc/proto.h"

static void put_u16(uint8_t *b, uint16_t v)
{
	b[0] = (uint8_t)(v & 0xFFU);
	b[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *b, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		b[i] = (uint8_t)(v >> (8 * i));
	}
}

static void put_u64(uint8_t *b, uint64_t v)
{
	for (int i = 0; i < 8; i++) {
		b[i] = (uint8_t)(v >> (8 * i));
	}
}

static uint16_t get_u16(const uint8_t *b)
{
	return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static uint32_t get_u32(const uint8_t *b)
{
	uint32_t v = 0U;

	for (int i = 0; i < 4; i++) {
		v |= (uint32_t)b[i] << (8 * i);
	}
	return v;
}

static uint64_t get_u64(const uint8_t *b)
{
	uint64_t v = 0U;

	for (int i = 0; i < 8; i++) {
		v |= (uint64_t)b[i] << (8 * i);
	}
	return v;
}

void rc_log_encode(const struct rc_log_record *r, uint8_t *out)
{
	memset(out, 0, RC_LOG_RECORD_SIZE);
	put_u16(&out[0], RC_LOG_MAGIC);
	out[2] = RC_LOG_VERSION;
	out[3] = r->type;
	put_u32(&out[4], r->seq);
	put_u32(&out[8], r->io_time_ms);
	put_u64(&out[12], r->mono_ms);
	put_u64(&out[20], r->wall_ms);
	out[28] = r->slot;
	out[29] = r->role;
	out[30] = r->mode;
	out[31] = r->flags;
	out[32] = r->active_slot;
	out[33] = r->io_fail;
	put_u16(&out[34], r->bit_results);
	put_u16(&out[36], r->step);
	out[38] = r->mask;
	out[39] = r->event;
	put_u32(&out[40], r->detail);
	put_u16(&out[44], r->io_boot_id);
	put_u16(&out[62], rc_crc16(out, 62U));
}

int rc_log_decode(const uint8_t *in, struct rc_log_record *r)
{
	if ((get_u16(&in[0]) != RC_LOG_MAGIC) || (in[2] != RC_LOG_VERSION) ||
	    ((in[3] != RC_LOG_SNAPSHOT) && (in[3] != RC_LOG_EVENT)) ||
	    (get_u16(&in[62]) != rc_crc16(in, 62U))) {
		return -1;
	}
	r->type = in[3];
	r->seq = get_u32(&in[4]);
	r->io_time_ms = get_u32(&in[8]);
	r->mono_ms = get_u64(&in[12]);
	r->wall_ms = get_u64(&in[20]);
	r->slot = in[28];
	r->role = in[29];
	r->mode = in[30];
	r->flags = in[31];
	r->active_slot = in[32];
	r->io_fail = in[33];
	r->bit_results = get_u16(&in[34]);
	r->step = get_u16(&in[36]);
	r->mask = in[38];
	r->event = in[39];
	r->detail = get_u32(&in[40]);
	r->io_boot_id = get_u16(&in[44]);
	return 0;
}

uint16_t rc_log_pack_bits(const uint8_t *results, int n)
{
	uint16_t packed = 0U;

	for (int i = 0; (i < n) && (i < 8); i++) {
		packed |= (uint16_t)((results[i] & 3U) << (2 * i));
	}
	return packed;
}

uint8_t rc_log_bit_result(uint16_t packed, int item)
{
	return (uint8_t)((packed >> (2 * item)) & 3U);
}
