#include <string.h>

#include "check.h"
#include "rc/proto.h"

static void test_crc16_check_value(void)
{
	const char *s = "123456789";

	CHECK(rc_crc16((const uint8_t *)s, strlen(s)) == 0x29B1U);
}

static void test_crc16_empty(void)
{
	CHECK(rc_crc16(NULL, 0U) == 0xFFFFU);
}

static size_t feed_all(struct rc_parser *p, const uint8_t *buf, size_t n,
		       struct rc_frame *frames, size_t max_frames)
{
	size_t count = 0U;

	for (size_t i = 0; i < n; i++) {
		struct rc_frame f;

		if (!rc_parser_feed(p, buf[i], &f)) {
			continue;
		}
		do {
			if (count < max_frames) {
				frames[count++] = f;
			}
		} while (rc_parser_next(p, &f));
	}
	return count;
}

static void test_encode_layout(void)
{
	const uint8_t payload[2] = {0x11U, 0x22U};
	uint8_t out[RC_FRAME_MAX];
	size_t n = rc_frame_encode(RC_MSG_SET_OUTPUTS, payload, 2U, out, sizeof(out));
	uint16_t crc = rc_crc16(&out[1], 4U);

	CHECK(n == 7U);
	CHECK(out[0] == RC_SYNC);
	CHECK(out[1] == RC_MSG_SET_OUTPUTS);
	CHECK(out[2] == 2U);
	CHECK(out[3] == 0x11U && out[4] == 0x22U);
	CHECK(out[5] == (uint8_t)(crc & 0xFFU));
	CHECK(out[6] == (uint8_t)(crc >> 8));
}

static void test_encode_rejects_bad_input(void)
{
	uint8_t payload[RC_MAX_PAYLOAD + 1U] = {0};
	uint8_t out[RC_FRAME_MAX + 1U];

	CHECK(rc_frame_encode(1U, payload, RC_MAX_PAYLOAD + 1U, out, sizeof(out)) == 0U);
	CHECK(rc_frame_encode(1U, payload, 4U, out, 8U) == 0U);
	CHECK(rc_frame_encode(1U, NULL, 1U, out, sizeof(out)) == 0U);
}

static void test_parser_roundtrip(void)
{
	const uint8_t payload[3] = {1U, 2U, 3U};
	uint8_t buf[RC_FRAME_MAX];
	size_t n = rc_frame_encode(RC_MSG_HEARTBEAT, payload, 3U, buf, sizeof(buf));
	struct rc_parser p;
	struct rc_frame frames[2];

	rc_parser_init(&p);
	CHECK(feed_all(&p, buf, n, frames, 2U) == 1U);
	CHECK(frames[0].type == RC_MSG_HEARTBEAT);
	CHECK(frames[0].len == 3U);
	CHECK(memcmp(frames[0].payload, payload, 3U) == 0);
}

static void test_parser_empty_payload(void)
{
	uint8_t buf[RC_FRAME_MAX];
	size_t n = rc_frame_encode(RC_MSG_STATUS, NULL, 0U, buf, sizeof(buf));
	struct rc_parser p;
	struct rc_frame frames[1];

	rc_parser_init(&p);
	CHECK(feed_all(&p, buf, n, frames, 1U) == 1U);
	CHECK(frames[0].len == 0U);
}

static void test_parser_noise_then_frame(void)
{
	/* Noise with a stray sync byte whose "length" (0x40) is too big, then a valid frame. */
	uint8_t buf[64] = {0x00U, 0xFFU, 0x13U, RC_SYNC, 0x07U, 0x40U, 0x99U, 0x3CU};
	const uint8_t payload[1] = {0x5AU};
	size_t n = 8U + rc_frame_encode(RC_MSG_SET_OUTPUTS, payload, 1U, &buf[8], sizeof(buf) - 8U);
	struct rc_parser p;
	struct rc_frame frames[2];

	rc_parser_init(&p);
	CHECK(feed_all(&p, buf, n, frames, 2U) == 1U);
	CHECK(p.len_errors == 1U);
	CHECK(frames[0].type == RC_MSG_SET_OUTPUTS);
	CHECK(frames[0].payload[0] == 0x5AU);
}

static void test_parser_bad_crc_counted_and_recovers(void)
{
	const uint8_t payload[1] = {0x01U};
	uint8_t buf[2 * RC_FRAME_MAX];
	size_t n1 = rc_frame_encode(RC_MSG_SET_OUTPUTS, payload, 1U, buf, sizeof(buf));
	size_t n2;
	struct rc_parser p;
	struct rc_frame frames[2];

	buf[3] ^= 0xFFU; /* corrupt the payload of the first frame */
	n2 = rc_frame_encode(RC_MSG_SET_OUTPUTS, payload, 1U, &buf[n1], sizeof(buf) - n1);
	rc_parser_init(&p);
	CHECK(feed_all(&p, buf, n1 + n2, frames, 2U) == 1U);
	CHECK(p.crc_errors == 1U);
	CHECK(frames[0].payload[0] == 0x01U);
}

static void test_parser_rejects_oversize_len(void)
{
	const uint8_t bad[3] = {RC_SYNC, RC_MSG_HEARTBEAT, RC_MAX_PAYLOAD + 1U};
	struct rc_parser p;
	struct rc_frame frames[1];

	rc_parser_init(&p);
	CHECK(feed_all(&p, bad, 3U, frames, 1U) == 0U);
	CHECK(p.len_errors == 1U);
}

static void test_parser_back_to_back(void)
{
	const uint8_t a[1] = {0xAAU};
	const uint8_t b[1] = {0xBBU};
	uint8_t buf[2 * RC_FRAME_MAX];
	size_t n = rc_frame_encode(RC_MSG_SET_OUTPUTS, a, 1U, buf, sizeof(buf));
	struct rc_parser p;
	struct rc_frame frames[2];

	n += rc_frame_encode(RC_MSG_SET_OUTPUTS, b, 1U, &buf[n], sizeof(buf) - n);
	rc_parser_init(&p);
	CHECK(feed_all(&p, buf, n, frames, 2U) == 2U);
	CHECK(frames[0].payload[0] == 0xAAU && frames[1].payload[0] == 0xBBU);
}

static bool decode_one(const uint8_t *buf, size_t n, struct rc_frame *f)
{
	struct rc_parser p;

	rc_parser_init(&p);
	return feed_all(&p, buf, n, f, 1U) == 1U;
}

static void test_heartbeat_roundtrip(void)
{
	struct rc_heartbeat in = {.seq = 0x01020304U, .role = RC_ROLE_ACTIVE, .healthy = 1U};
	struct rc_heartbeat out = {0};
	uint8_t buf[RC_FRAME_MAX];
	struct rc_frame f;
	size_t n = rc_encode_heartbeat(&in, buf, sizeof(buf));

	CHECK(n == RC_FRAME_OVERHEAD + 6U);
	CHECK(buf[3] == 0x04U); /* little-endian seq */
	CHECK(decode_one(buf, n, &f));
	CHECK(rc_decode_heartbeat(&f, &out) == 0);
	CHECK(out.seq == in.seq && out.role == in.role && out.healthy == 1U);
}

static void test_status_roundtrip(void)
{
	struct rc_status in = {.seq = 7U, .slot = RC_SLOT_B, .granted_role = RC_ROLE_STANDBY,
			       .active_slot = RC_SLOT_A, .mode = RC_MODE_TEST, .io_fail = 0x21U};
	struct rc_status out = {0};
	uint8_t buf[RC_FRAME_MAX];
	struct rc_frame f;
	size_t n = rc_encode_status(&in, buf, sizeof(buf));

	CHECK(n == RC_FRAME_OVERHEAD + 9U);
	CHECK(decode_one(buf, n, &f));
	CHECK(rc_decode_status(&f, &out) == 0);
	CHECK(out.seq == 7U && out.slot == RC_SLOT_B);
	CHECK(out.granted_role == RC_ROLE_STANDBY && out.active_slot == RC_SLOT_A);
	CHECK(out.mode == RC_MODE_TEST && out.io_fail == 0x21U);
}

static void test_set_outputs_roundtrip(void)
{
	struct rc_set_outputs in = {.mask = 0x81U};
	struct rc_set_outputs out = {0};
	uint8_t buf[RC_FRAME_MAX];
	struct rc_frame f;
	size_t n = rc_encode_set_outputs(&in, buf, sizeof(buf));

	CHECK(decode_one(buf, n, &f));
	CHECK(rc_decode_set_outputs(&f, &out) == 0);
	CHECK(out.mask == 0x81U);
}

static void test_peer_roundtrip(void)
{
	struct rc_peer in = {.seq = 0xFFFFFFFFU, .slot = RC_SLOT_A, .role = RC_ROLE_ACTIVE,
			     .referee_ok = 1U, .step = 0xBEEFU, .healthy = 1U, .test_mask = 0x55U};
	struct rc_peer out = {0};
	uint8_t buf[RC_FRAME_MAX];
	struct rc_frame f;
	size_t n = rc_encode_peer(&in, buf, sizeof(buf));

	CHECK(n == RC_FRAME_OVERHEAD + 11U);
	CHECK(decode_one(buf, n, &f));
	CHECK(rc_decode_peer(&f, &out) == 0);
	CHECK(out.seq == in.seq && out.slot == in.slot && out.role == in.role);
	CHECK(out.referee_ok == 1U && out.step == 0xBEEFU);
	CHECK(out.healthy == 1U && out.test_mask == 0x55U);
}

static void test_run_bit_roundtrip(void)
{
	uint8_t buf[RC_FRAME_MAX];
	struct rc_frame f;
	size_t n = rc_encode_run_bit(buf, sizeof(buf));

	CHECK(n == RC_FRAME_OVERHEAD);
	CHECK(decode_one(buf, n, &f));
	CHECK(rc_decode_run_bit(&f) == 0);
	f.len = 1U;
	CHECK(rc_decode_run_bit(&f) == -1);
	f.type = RC_MSG_STATUS;
	f.len = 0U;
	CHECK(rc_decode_run_bit(&f) == -1);
}

static void test_decode_rejects_part1_lengths(void)
{
	struct rc_frame hb = {.type = RC_MSG_HEARTBEAT, .len = 5U};
	struct rc_frame st = {.type = RC_MSG_STATUS, .len = 7U};
	struct rc_frame pe = {.type = RC_MSG_PEER, .len = 9U};
	struct rc_heartbeat h;
	struct rc_status s;
	struct rc_peer p;

	CHECK(rc_decode_heartbeat(&hb, &h) == -1);
	CHECK(rc_decode_status(&st, &s) == -1);
	CHECK(rc_decode_peer(&pe, &p) == -1);
}

static void test_decode_rejects_wrong_type_or_len(void)
{
	struct rc_frame f = {.type = RC_MSG_STATUS, .len = 5U};
	struct rc_heartbeat hb;
	struct rc_status st;

	CHECK(rc_decode_heartbeat(&f, &hb) == -1);
	CHECK(rc_decode_status(&f, &st) == -1);
}

static void test_parser_false_sync_does_not_swallow_frames(void)
{
	/* Noise ending in a plausible header: sync, type 1, len 16. */
	uint8_t buf[64] = {0x3CU, RC_SYNC, 0x01U, 0x10U};
	size_t n = 4U;
	struct rc_parser p;
	struct rc_frame frames[4];

	for (uint32_t i = 0; i < 3U; i++) {
		struct rc_heartbeat hb = {.seq = i, .role = RC_ROLE_STANDBY};

		n += rc_encode_heartbeat(&hb, &buf[n], sizeof(buf) - n);
	}
	rc_parser_init(&p);
	CHECK(feed_all(&p, buf, n, frames, 4U) == 3U);
	CHECK(frames[0].type == RC_MSG_HEARTBEAT && frames[0].payload[0] == 0U);
	CHECK(frames[2].payload[0] == 2U);
}

int main(void)
{
	test_crc16_check_value();
	test_crc16_empty();
	test_encode_layout();
	test_encode_rejects_bad_input();
	test_parser_roundtrip();
	test_parser_empty_payload();
	test_parser_noise_then_frame();
	test_parser_bad_crc_counted_and_recovers();
	test_parser_rejects_oversize_len();
	test_parser_back_to_back();
	test_parser_false_sync_does_not_swallow_frames();
	test_heartbeat_roundtrip();
	test_status_roundtrip();
	test_set_outputs_roundtrip();
	test_peer_roundtrip();
	test_decode_rejects_wrong_type_or_len();
	test_run_bit_roundtrip();
	test_decode_rejects_part1_lengths();
	return CHECK_DONE();
}
