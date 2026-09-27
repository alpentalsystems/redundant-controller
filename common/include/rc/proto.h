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

#endif /* RC_PROTO_H_ */
