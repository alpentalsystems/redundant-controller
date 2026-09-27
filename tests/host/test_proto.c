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

		if (rc_parser_feed(p, buf[i], &f) && count < max_frames) {
			frames[count++] = f;
		}
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
	return CHECK_DONE();
}
