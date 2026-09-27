#include <string.h>

#include "check.h"
#include "rc/logrec.h"
#include "rc/proto.h"

static struct rc_log_record sample(void)
{
	struct rc_log_record r;

	memset(&r, 0, sizeof(r));
	r.type = RC_LOG_EVENT;
	r.seq = 0x01020304U;
	r.io_time_ms = 123456U;
	r.mono_ms = 9876543210ULL;
	r.wall_ms = 1790000000123ULL;
	r.slot = RC_SLOT_B;
	r.role = RC_ROLE_ACTIVE;
	r.mode = RC_MODE_TEST;
	r.flags = RC_LOG_F_HEALTHY | RC_LOG_F_IO_TIME_VALID;
	r.active_slot = RC_SLOT_B;
	r.io_fail = 0x21U;
	r.bit_results = 0x0350U;
	r.step = 513U;
	r.mask = 0x55U;
	r.event = RC_LOG_EV_ROLE_CHANGED;
	r.detail = RC_ROLE_ACTIVE;
	return r;
}

/* Rewrites the CRC after a test changes a byte on purpose. */
static void fix_crc(uint8_t *b)
{
	uint16_t crc = rc_crc16(b, 62U);

	b[62] = (uint8_t)(crc & 0xFFU);
	b[63] = (uint8_t)(crc >> 8);
}

static void test_roundtrip(void)
{
	struct rc_log_record in = sample();
	struct rc_log_record out;
	uint8_t b[RC_LOG_RECORD_SIZE];

	memset(&out, 0, sizeof(out));
	rc_log_encode(&in, b);
	CHECK(b[0] == 0x52U && b[1] == 0x4CU && b[2] == RC_LOG_VERSION);
	CHECK(b[4] == 0x04U); /* little-endian seq */
	for (size_t i = 44U; i < 62U; i++) {
		CHECK(b[i] == 0U);
	}
	CHECK(rc_log_decode(b, &out) == 0);
	CHECK(out.type == in.type && out.seq == in.seq && out.io_time_ms == in.io_time_ms);
	CHECK(out.mono_ms == in.mono_ms && out.wall_ms == in.wall_ms);
	CHECK(out.slot == in.slot && out.role == in.role && out.mode == in.mode);
	CHECK(out.flags == in.flags && out.active_slot == in.active_slot);
	CHECK(out.io_fail == in.io_fail && out.bit_results == in.bit_results);
	CHECK(out.step == in.step && out.mask == in.mask);
	CHECK(out.event == in.event && out.detail == in.detail);
}

static void test_flipped_byte_rejected(void)
{
	struct rc_log_record in = sample();
	struct rc_log_record out;
	uint8_t b[RC_LOG_RECORD_SIZE];

	rc_log_encode(&in, b);
	b[20] ^= 0x01U;
	CHECK(rc_log_decode(b, &out) == -1);
}

static void test_bad_magic_version_type(void)
{
	struct rc_log_record in = sample();
	struct rc_log_record out;
	uint8_t b[RC_LOG_RECORD_SIZE];

	rc_log_encode(&in, b);
	b[0] = 0x00U;
	fix_crc(b);
	CHECK(rc_log_decode(b, &out) == -1);
	rc_log_encode(&in, b);
	b[2] = 2U;
	fix_crc(b);
	CHECK(rc_log_decode(b, &out) == -1);
	rc_log_encode(&in, b);
	b[3] = 3U;
	fix_crc(b);
	CHECK(rc_log_decode(b, &out) == -1);
}

static void test_empty_slot_rejected(void)
{
	struct rc_log_record out;
	uint8_t b[RC_LOG_RECORD_SIZE];

	memset(b, 0, sizeof(b));
	CHECK(rc_log_decode(b, &out) == -1);
}

static void test_bit_packing(void)
{
	const uint8_t results[5] = {0U, 0U, 1U, 1U, RC_LOG_BIT_NOT_RUN};

	CHECK(rc_log_pack_bits(results, 5) == 0x0350U);
	CHECK(rc_log_bit_result(0x0350U, 2) == 1U);
	CHECK(rc_log_bit_result(0x0350U, 4) == RC_LOG_BIT_NOT_RUN);
	CHECK(rc_log_bit_result(0x0350U, 0) == 0U);
}

int main(void)
{
	test_roundtrip();
	test_flipped_byte_rejected();
	test_bad_magic_version_type();
	test_empty_slot_rejected();
	test_bit_packing();
	return CHECK_DONE();
}
