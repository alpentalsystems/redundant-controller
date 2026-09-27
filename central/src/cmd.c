#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmd.h"
#include "rc/proto.h"

struct out {
	char *buf;
	size_t size;
	size_t n;
};

/* Appends to the reply; replies are sized to fit CMD_REPLY_MAX. */
static void put(struct out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(struct out *o, const char *fmt, ...)
{
	va_list ap;
	int w;

	if ((o->n + 1U) >= o->size) {
		return;
	}
	va_start(ap, fmt);
	w = vsnprintf(&o->buf[o->n], o->size - o->n, fmt, ap);
	va_end(ap);
	if (w < 0) {
		o->buf[o->n] = '\0';
		return;
	}
	o->n += (size_t)w;
	if (o->n >= o->size) {
		o->n = o->size - 1U;
	}
}

static const char *slot_str(uint8_t slot)
{
	return (slot == RC_SLOT_A) ? "A" : ((slot == RC_SLOT_B) ? "B" : "none");
}

static const char *role_str(uint8_t role)
{
	switch (role) {
	case RC_ROLE_ACTIVE:
		return "active";
	case RC_ROLE_STANDBY:
		return "standby";
	default:
		return "unknown";
	}
}

static const char *mode_str(uint8_t mode)
{
	return (mode == RC_MODE_TEST) ? "test" : "operational";
}

static const char *tf(bool b)
{
	return b ? "true" : "false";
}

static size_t reply_error(const char *msg, char *buf, size_t size)
{
	struct out o = {buf, size, 0U};

	put(&o, "{\"ok\":false,\"error\":\"%s\"}\n", msg);
	return o.n;
}

static size_t reply_ok(char *buf, size_t size)
{
	struct out o = {buf, size, 0U};

	put(&o, "{\"ok\":true}\n");
	return o.n;
}

static size_t reply_not_active(const struct cmd_view *v, char *buf, size_t size)
{
	struct out o = {buf, size, 0U};

	put(&o, "{\"ok\":false,\"error\":\"not active\",\"active\":\"%s\"}\n",
	    slot_str(v->active_slot));
	return o.n;
}

static size_t reply_status(const struct cmd_view *v, char *buf, size_t size)
{
	struct out o = {buf, size, 0U};

	put(&o,
	    "{\"ok\":true,\"slot\":\"%s\",\"role\":\"%s\",\"mode\":\"%s\",\"healthy\":%s,"
	    "\"active\":\"%s\",\"referee\":%s,\"peer\":%s,\"peer_healthy\":%s,\"fault\":%s,"
	    "\"step\":%u,\"mask\":%u,\"io_fail\":%u}\n",
	    slot_str(v->slot), role_str(v->role), mode_str(v->mode), tf(v->healthy),
	    slot_str(v->active_slot), tf(v->referee_ok), tf(v->peer_ok), tf(v->peer_healthy),
	    tf(v->fault), (unsigned)v->step, (unsigned)v->mask, (unsigned)v->io_fail);
	return o.n;
}

static void put_report(struct out *o, const char *key, const struct bit_report *r,
		       int64_t now_ms)
{
	if ((r == NULL) || !r->valid) {
		put(o, "\"%s\":null", key);
		return;
	}
	put(o, "\"%s\":{\"age_ms\":%lld,\"items\":[", key, (long long)(now_ms - r->at_ms));
	for (int i = 0; i < BIT_ITEMS; i++) {
		char value[64];

		bit_item_value(r, i, value, sizeof(value));
		put(o, "%s{\"name\":\"%s\",\"result\":\"%s\",\"critical\":%s,\"value\":\"%s\"}",
		    (i == 0) ? "" : ",", bit_item_name(i), bit_result_name(r->result[i]),
		    tf(bit_is_critical(i)), value);
	}
	put(o, "]}");
}

static size_t reply_bit(const struct cmd_view *v, char *buf, size_t size)
{
	struct out o = {buf, size, 0U};

	put(&o, "{\"ok\":true,\"healthy\":%s,", tf(v->healthy));
	put_report(&o, "pbit", v->pbit, v->now_ms);
	put(&o, ",");
	put_report(&o, "cbit", v->cbit, v->now_ms);
	put(&o, ",\"io_fail\":%u}\n", (unsigned)v->io_fail);
	return o.n;
}

/* One or two hex digits, optional 0x prefix. */
static bool parse_mask(const char *s, uint8_t *v)
{
	size_t len;

	if ((s[0] == '0') && ((s[1] == 'x') || (s[1] == 'X'))) {
		s += 2;
	}
	len = strlen(s);
	if ((len == 0U) || (len > 2U)) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		if (isxdigit((unsigned char)s[i]) == 0) {
			return false;
		}
	}
	*v = (uint8_t)strtoul(s, NULL, 16);
	return true;
}

void cmd_linebuf_init(struct cmd_linebuf *lb)
{
	lb->n = 0U;
}

enum cmd_feed cmd_linebuf_feed(struct cmd_linebuf *lb, char c, char *line)
{
	if (c == '\n') {
		size_t n = lb->n;

		if ((n > 0U) && (lb->buf[n - 1U] == '\r')) {
			n--;
		}
		memcpy(line, lb->buf, n);
		line[n] = '\0';
		lb->n = 0U;
		return CMD_FEED_LINE;
	}
	if (lb->n == CMD_LINE_MAX) {
		lb->n = 0U;
		return CMD_FEED_TOO_LONG;
	}
	lb->buf[lb->n++] = c;
	return CMD_FEED_NONE;
}

size_t cmd_handle(const char *line, const struct cmd_view *v, struct cmd_result *res, char *out,
		  size_t out_size)
{
	char word[16] = "";
	char arg[CMD_LINE_MAX + 1U] = "";
	char extra;
	int n = sscanf(line, " %15s %128s %c", word, arg, &extra);
	bool leds;
	bool simple;

	res->action = CMD_ACT_NONE;
	res->leds = 0U;
	if ((n == 1) && (strcmp(word, "STATUS") == 0)) {
		return reply_status(v, out, out_size);
	}
	if ((n == 1) && (strcmp(word, "BIT") == 0)) {
		return reply_bit(v, out, out_size);
	}
	leds = ((n == 1) || (n == 2)) && (strcmp(word, "LEDS") == 0);
	simple = (n == 1) && ((strcmp(word, "LAMP_TEST") == 0) || (strcmp(word, "RUN_BIT") == 0));
	if (!leds && !simple) {
		return reply_error("unknown command", out, out_size);
	}
	if (v->role != RC_ROLE_ACTIVE) {
		return reply_not_active(v, out, out_size);
	}
	if (v->mode != RC_MODE_TEST) {
		return reply_error("operational mode", out, out_size);
	}
	/* Without STATUS the grant may have moved; the I/O card would drop our outputs. */
	if (!v->referee_ok) {
		return reply_error("referee lost", out, out_size);
	}
	if (leds) {
		if ((n != 2) || !parse_mask(arg, &res->leds)) {
			return reply_error("bad value", out, out_size);
		}
		res->action = CMD_ACT_SET_LEDS;
	} else if (strcmp(word, "LAMP_TEST") == 0) {
		res->action = CMD_ACT_LAMP_TEST;
	} else {
		res->action = CMD_ACT_RUN_BIT;
	}
	return reply_ok(out, out_size);
}

size_t cmd_too_long(char *out, size_t out_size)
{
	return reply_error("line too long", out, out_size);
}
