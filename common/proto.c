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

enum {
	ST_SYNC,
	ST_TYPE,
	ST_LEN,
	ST_PAYLOAD,
	ST_CRC_LO,
	ST_CRC_HI,
};

void rc_parser_init(struct rc_parser *p)
{
	memset(p, 0, sizeof(*p));
	p->state = ST_SYNC;
}

bool rc_parser_feed(struct rc_parser *p, uint8_t byte, struct rc_frame *out)
{
	switch (p->state) {
	case ST_SYNC:
		if (byte == RC_SYNC) {
			p->crc = 0xFFFFU;
			p->state = ST_TYPE;
		}
		return false;
	case ST_TYPE:
		p->type = byte;
		p->crc = crc_update(p->crc, byte);
		p->state = ST_LEN;
		return false;
	case ST_LEN:
		if (byte > RC_MAX_PAYLOAD) {
			p->len_errors++;
			p->state = ST_SYNC;
			return false;
		}
		p->len = byte;
		p->idx = 0U;
		p->crc = crc_update(p->crc, byte);
		p->state = (byte == 0U) ? ST_CRC_LO : ST_PAYLOAD;
		return false;
	case ST_PAYLOAD:
		p->payload[p->idx++] = byte;
		p->crc = crc_update(p->crc, byte);
		if (p->idx == p->len) {
			p->state = ST_CRC_LO;
		}
		return false;
	case ST_CRC_LO:
		p->crc_lo = byte;
		p->state = ST_CRC_HI;
		return false;
	case ST_CRC_HI:
		p->state = ST_SYNC;
		if ((uint16_t)(p->crc_lo | ((uint16_t)byte << 8)) != p->crc) {
			p->crc_errors++;
			return false;
		}
		out->type = p->type;
		out->len = p->len;
		memcpy(out->payload, p->payload, p->len);
		return true;
	default:
		p->state = ST_SYNC;
		return false;
	}
}
