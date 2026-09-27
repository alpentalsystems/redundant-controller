#include "rc/proto.h"

#include <string.h>

static uint16_t crc_update(uint16_t crc, uint8_t byte)
{
	crc ^= (uint16_t)((uint16_t)byte << 8);
	for (int bit = 0; bit < 8; bit++) {
		if ((crc & 0x8000U) != 0U) {
			crc = (uint16_t)((crc << 1) ^ 0x1021U);
		} else {
			crc = (uint16_t)(crc << 1);
		}
	}
	return crc;
}

uint16_t rc_crc16(const uint8_t *data, size_t len)
{
	uint16_t crc = 0xFFFFU;

	for (size_t i = 0; i < len; i++) {
		crc = crc_update(crc, data[i]);
	}
	return crc;
}

size_t rc_frame_encode(uint8_t type, const uint8_t *payload, uint8_t len, uint8_t *out,
		       size_t out_size)
{
	size_t total = RC_FRAME_OVERHEAD + (size_t)len;
	uint16_t crc;

	if ((len > RC_MAX_PAYLOAD) || (out_size < total) || ((len > 0U) && (payload == NULL))) {
		return 0U;
	}
	out[0] = RC_SYNC;
	out[1] = type;
	out[2] = len;
	if (len > 0U) {
		memcpy(&out[3], payload, len);
	}
	crc = rc_crc16(&out[1], (size_t)len + 2U);
	out[3U + len] = (uint8_t)(crc & 0xFFU);
	out[4U + len] = (uint8_t)(crc >> 8);
	return total;
}

static void drop(struct rc_parser *p, uint8_t count)
{
	memmove(p->buf, &p->buf[count], (size_t)(p->n - count));
	p->n = (uint8_t)(p->n - count);
}

static void enqueue(struct rc_parser *p, uint8_t type, uint8_t len, const uint8_t *payload)
{
	struct rc_frame *f;

	if (p->q_count == RC_PARSER_QUEUE) {
		p->queue_overflows++;
		return;
	}
	f = &p->queue[(p->q_head + p->q_count) % RC_PARSER_QUEUE];
	f->type = type;
	f->len = len;
	memcpy(f->payload, payload, len);
	p->q_count++;
}

static void scan(struct rc_parser *p)
{
	for (;;) {
		uint8_t skip = 0U;
		uint8_t len;
		uint8_t total;
		uint16_t rx;

		while ((skip < p->n) && (p->buf[skip] != RC_SYNC)) {
			skip++;
		}
		if (skip > 0U) {
			drop(p, skip);
		}
		if (p->n < 3U) {
			return;
		}
		len = p->buf[2];
		if (len > RC_MAX_PAYLOAD) {
			p->len_errors++;
			drop(p, 1U);
			continue;
		}
		total = (uint8_t)(RC_FRAME_OVERHEAD + len);
		if (p->n < total) {
			return;
		}
		rx = (uint16_t)((uint16_t)p->buf[3U + len] | ((uint16_t)p->buf[4U + len] << 8));
		if (rc_crc16(&p->buf[1], (size_t)len + 2U) != rx) {
			p->crc_errors++;
			drop(p, 1U);
			continue;
		}
		enqueue(p, p->buf[1], len, &p->buf[3]);
		drop(p, total);
	}
}

void rc_parser_init(struct rc_parser *p)
{
	memset(p, 0, sizeof(*p));
}

bool rc_parser_next(struct rc_parser *p, struct rc_frame *out)
{
	if (p->q_count == 0U) {
		return false;
	}
	*out = p->queue[p->q_head];
	p->q_head = (uint8_t)((p->q_head + 1U) % RC_PARSER_QUEUE);
	p->q_count--;
	return true;
}

bool rc_parser_feed(struct rc_parser *p, uint8_t byte, struct rc_frame *out)
{
	/* scan() leaves at most RC_FRAME_MAX - 1 bytes, so there is room for one more. */
	p->buf[p->n++] = byte;
	scan(p);
	return rc_parser_next(p, out);
}

static void put_u16(uint8_t *b, uint16_t v)
{
	b[0] = (uint8_t)(v & 0xFFU);
	b[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)(v & 0xFFU);
	b[1] = (uint8_t)((v >> 8) & 0xFFU);
	b[2] = (uint8_t)((v >> 16) & 0xFFU);
	b[3] = (uint8_t)(v >> 24);
}

static uint16_t get_u16(const uint8_t *b)
{
	return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static uint32_t get_u32(const uint8_t *b)
{
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
	       ((uint32_t)b[3] << 24);
}

size_t rc_encode_heartbeat(const struct rc_heartbeat *m, uint8_t *out, size_t out_size)
{
	uint8_t p[5];

	put_u32(p, m->seq);
	p[4] = m->role;
	return rc_frame_encode(RC_MSG_HEARTBEAT, p, sizeof(p), out, out_size);
}

size_t rc_encode_status(const struct rc_status *m, uint8_t *out, size_t out_size)
{
	uint8_t p[7];

	put_u32(p, m->seq);
	p[4] = m->slot;
	p[5] = m->granted_role;
	p[6] = m->active_slot;
	return rc_frame_encode(RC_MSG_STATUS, p, sizeof(p), out, out_size);
}

size_t rc_encode_set_outputs(const struct rc_set_outputs *m, uint8_t *out, size_t out_size)
{
	return rc_frame_encode(RC_MSG_SET_OUTPUTS, &m->mask, 1U, out, out_size);
}

size_t rc_encode_peer(const struct rc_peer *m, uint8_t *out, size_t out_size)
{
	uint8_t p[9];

	put_u32(p, m->seq);
	p[4] = m->slot;
	p[5] = m->role;
	p[6] = m->referee_ok;
	put_u16(&p[7], m->step);
	return rc_frame_encode(RC_MSG_PEER, p, sizeof(p), out, out_size);
}

int rc_decode_heartbeat(const struct rc_frame *f, struct rc_heartbeat *m)
{
	if ((f->type != RC_MSG_HEARTBEAT) || (f->len != 5U)) {
		return -1;
	}
	m->seq = get_u32(f->payload);
	m->role = f->payload[4];
	return 0;
}

int rc_decode_status(const struct rc_frame *f, struct rc_status *m)
{
	if ((f->type != RC_MSG_STATUS) || (f->len != 7U)) {
		return -1;
	}
	m->seq = get_u32(f->payload);
	m->slot = f->payload[4];
	m->granted_role = f->payload[5];
	m->active_slot = f->payload[6];
	return 0;
}

int rc_decode_set_outputs(const struct rc_frame *f, struct rc_set_outputs *m)
{
	if ((f->type != RC_MSG_SET_OUTPUTS) || (f->len != 1U)) {
		return -1;
	}
	m->mask = f->payload[0];
	return 0;
}

int rc_decode_peer(const struct rc_frame *f, struct rc_peer *m)
{
	if ((f->type != RC_MSG_PEER) || (f->len != 9U)) {
		return -1;
	}
	m->seq = get_u32(f->payload);
	m->slot = f->payload[4];
	m->role = f->payload[5];
	m->referee_ok = f->payload[6];
	m->step = get_u16(&f->payload[7]);
	return 0;
}
