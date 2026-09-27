#ifndef RC_PROTO_H_
#define RC_PROTO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RC_SYNC 0xA5U
#define RC_MAX_PAYLOAD 16U
/* sync, type, len, crc (2) */
#define RC_FRAME_OVERHEAD 5U
#define RC_FRAME_MAX (RC_FRAME_OVERHEAD + RC_MAX_PAYLOAD)

#define RC_MSG_HEARTBEAT 1U
#define RC_MSG_STATUS 2U
#define RC_MSG_SET_OUTPUTS 3U
#define RC_MSG_PEER 4U

#define RC_ROLE_UNKNOWN 0U
#define RC_ROLE_STANDBY 1U
#define RC_ROLE_ACTIVE 2U

#define RC_SLOT_A 0U
#define RC_SLOT_B 1U
#define RC_SLOT_NONE 0xFFU

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection. */
uint16_t rc_crc16(const uint8_t *data, size_t len);

struct rc_frame {
	uint8_t type;
	uint8_t len;
	uint8_t payload[RC_MAX_PAYLOAD];
};

/* Returns the frame length, or 0 if the payload or buffer is invalid. */
size_t rc_frame_encode(uint8_t type, const uint8_t *payload, uint8_t len, uint8_t *out,
		       size_t out_size);

struct rc_parser {
	uint8_t state;
	uint8_t type;
	uint8_t len;
	uint8_t idx;
	uint8_t crc_lo;
	uint16_t crc;
	uint8_t payload[RC_MAX_PAYLOAD];
	uint32_t crc_errors;
	uint32_t len_errors;
};

void rc_parser_init(struct rc_parser *p);

/* Feeds one byte; returns true when *out holds a complete, valid frame. */
bool rc_parser_feed(struct rc_parser *p, uint8_t byte, struct rc_frame *out);

#endif /* RC_PROTO_H_ */
