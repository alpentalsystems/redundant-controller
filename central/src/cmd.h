#ifndef CMD_H_
#define CMD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bit.h"

#define CMD_LINE_MAX 128U
#define CMD_REPLY_MAX 2048U

enum cmd_action { CMD_ACT_NONE, CMD_ACT_SET_LEDS, CMD_ACT_LAMP_TEST, CMD_ACT_RUN_BIT };

/* The daemon state a command can see. */
struct cmd_view {
	uint8_t slot;
	uint8_t role;
	uint8_t mode;
	uint8_t active_slot;
	bool healthy;
	bool referee_ok;
	bool peer_ok;
	bool peer_healthy;
	bool fault;
	uint16_t step;
	uint8_t mask;
	uint8_t io_fail;
	const struct bit_report *pbit;
	const struct bit_report *cbit;
	int64_t now_ms;
};

struct cmd_result {
	enum cmd_action action;
	uint8_t leds;
};

struct cmd_linebuf {
	char buf[CMD_LINE_MAX];
	size_t n;
};

enum cmd_feed { CMD_FEED_NONE, CMD_FEED_LINE, CMD_FEED_TOO_LONG };

void cmd_linebuf_init(struct cmd_linebuf *lb);

/*
 * Feeds one received byte. On CMD_FEED_LINE, line (CMD_LINE_MAX + 1 bytes)
 * holds the line without its LF or CRLF. CMD_FEED_TOO_LONG: more than
 * CMD_LINE_MAX bytes before LF.
 */
enum cmd_feed cmd_linebuf_feed(struct cmd_linebuf *lb, char c, char *line);

/* Handles one line; writes a JSON reply ending in '\n' and returns its length. */
size_t cmd_handle(const char *line, const struct cmd_view *v, struct cmd_result *res, char *out,
		  size_t out_size);

size_t cmd_too_long(char *out, size_t out_size);

#endif /* CMD_H_ */
