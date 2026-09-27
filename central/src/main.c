#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "bit.h"
#include "cmd.h"
#include "rc/proto.h"
#include "role.h"

#define SERIAL_DEV "/dev/serial0"
#define PEER_IFACE "eth0"
#define PEER_GROUP "ff02::1"
#define PEER_PORT 47000
#define HEARTBEAT_PERIOD_MS 20
#define TCP_PORT 5000
#define TCP_CLIENTS 4
#define HWMON_DIR "/sys/class/hwmon"
#define TEMP_PATH "/sys/class/thermal/thermal_zone0/temp"
/* On-demand BIT runs at most this often; extra RUN_BIT commands reuse the result. */
#define RUN_BIT_MIN_INTERVAL_MS 500

struct client {
	int fd;
	struct cmd_linebuf lb;
};

struct daemon {
	int serial;
	int peer;
	int listener;
	struct sockaddr_in6 group;
	struct rc_parser parser;
	struct role_state s;
	struct bit_health health;
	struct bit_report pbit;
	struct bit_report cbit;
	uint32_t errors_at_check;
	int64_t last_run_bit_ms;
	int64_t last_hb_ms;
	int64_t max_hb_interval_ms;
	uint32_t hb_seq;
	uint32_t peer_seq;
	bool peer_send_ok;
	bool peer_recv_ok;
	char uv_path[96];
	struct client clients[TCP_CLIENTS];
};

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

static void die(const char *what)
{
	fprintf(stderr, "fatal: %s: %s\n", what, strerror(errno));
	exit(1);
}

static int open_serial(void)
{
	struct termios tio;
	int fd = open(SERIAL_DEV, O_RDWR | O_NOCTTY | O_NONBLOCK);

	if (fd < 0) {
		die("open " SERIAL_DEV);
	}
	if (tcgetattr(fd, &tio) != 0) {
		die("tcgetattr");
	}
	cfmakeraw(&tio);
	cfsetispeed(&tio, B115200);
	cfsetospeed(&tio, B115200);
	tio.c_cflag |= CLOCAL | CREAD;
	if (tcsetattr(fd, TCSANOW, &tio) != 0) {
		die("tcsetattr");
	}
	return fd;
}

/* Returns the socket, or -1 (logged) if the cross-link cannot be set up. */
static int open_peer(struct sockaddr_in6 *group)
{
	unsigned int ifindex = if_nametoindex(PEER_IFACE);
	struct sockaddr_in6 local = {.sin6_family = AF_INET6, .sin6_port = htons(PEER_PORT),
				     .sin6_addr = IN6ADDR_ANY_INIT};
	int one = 1;
	int zero = 0;
	const char *step = NULL;
	int fd;

	if (ifindex == 0U) {
		fprintf(stderr, "error: cross-link disabled: if_nametoindex " PEER_IFACE ": %s\n",
			strerror(errno));
		return -1;
	}
	fd = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		step = "socket";
	} else if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
		step = "SO_REUSEADDR";
	} else if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, PEER_IFACE, sizeof(PEER_IFACE)) != 0) {
		step = "SO_BINDTODEVICE";
	} else if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
		step = "bind";
	} else if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifindex, sizeof(ifindex)) != 0) {
		step = "IPV6_MULTICAST_IF";
	} else if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &zero, sizeof(zero)) != 0) {
		step = "IPV6_MULTICAST_LOOP";
	}
	if (step != NULL) {
		fprintf(stderr, "error: cross-link disabled: %s: %s\n", step, strerror(errno));
		if (fd >= 0) {
			close(fd);
		}
		return -1;
	}
	memset(group, 0, sizeof(*group));
	group->sin6_family = AF_INET6;
	group->sin6_port = htons(PEER_PORT);
	group->sin6_scope_id = ifindex;
	if (inet_pton(AF_INET6, PEER_GROUP, &group->sin6_addr) != 1) {
		die("inet_pton");
	}
	return fd;
}

/* Returns the listening socket, or -1 (logged): commands are not needed for control. */
static int open_listener(void)
{
	struct sockaddr_in6 addr = {.sin6_family = AF_INET6, .sin6_port = htons(TCP_PORT),
				    .sin6_addr = IN6ADDR_ANY_INIT};
	int one = 1;
	int zero = 0;
	const char *step = NULL;
	int fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);

	if (fd < 0) {
		step = "socket";
	} else if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
		step = "SO_REUSEADDR";
	} else if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero)) != 0) {
		step = "IPV6_V6ONLY";
	} else if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		step = "bind";
	} else if (listen(fd, TCP_CLIENTS) != 0) {
		step = "listen";
	}
	if (step != NULL) {
		fprintf(stderr, "error: tcp server disabled: %s: %s\n", step, strerror(errno));
		if (fd >= 0) {
			close(fd);
		}
		return -1;
	}
	return fd;
}

static bool read_long(const char *path, long *v)
{
	FILE *f = fopen(path, "r");
	bool ok;

	if (f == NULL) {
		return false;
	}
	ok = (fscanf(f, "%ld", v) == 1);
	fclose(f);
	return ok;
}

/* Finds the under-voltage flag of the rpi_volt hwmon device. */
static int find_undervoltage_path(char *path, size_t size)
{
	for (int i = 0; i < 16; i++) {
		char name_path[64];
		char name[32] = "";
		FILE *f;

		snprintf(name_path, sizeof(name_path), HWMON_DIR "/hwmon%d/name", i);
		f = fopen(name_path, "r");
		if (f == NULL) {
			continue;
		}
		if (fgets(name, sizeof(name), f) == NULL) {
			name[0] = '\0';
		}
		fclose(f);
		if (strncmp(name, "rpi_volt", 8) == 0) {
			snprintf(path, size, HWMON_DIR "/hwmon%d/in0_lcrit_alarm", i);
			return 0;
		}
	}
	path[0] = '\0';
	return -1;
}

static const char *role_name(uint8_t role)
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

static char slot_name(uint8_t slot)
{
	return (slot == RC_SLOT_A) ? 'A' : ((slot == RC_SLOT_B) ? 'B' : '-');
}

static void log_events(unsigned ev, const struct role_state *s, int64_t t)
{
	static const struct {
		unsigned bit;
		const char *name;
	} names[] = {
		{ROLE_EV_SLOT_LEARNED, "slot_learned"},   {ROLE_EV_ROLE_CHANGED, "role_changed"},
		{ROLE_EV_REFEREE_LOST, "referee_lost"},   {ROLE_EV_REFEREE_BACK, "referee_back"},
		{ROLE_EV_PEER_LOST, "peer_lost"},         {ROLE_EV_PEER_BACK, "peer_back"},
		{ROLE_EV_FAULT_SET, "referee_fault"},     {ROLE_EV_FAULT_CLEARED, "fault_cleared"},
		{ROLE_EV_MODE_CHANGED, "mode_changed"},
	};

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if ((ev & names[i].bit) != 0U) {
			printf("t=%lld slot=%c role=%s mode=%s step=%u event=%s\n", (long long)t,
			       slot_name(s->slot), role_name(s->role),
			       (s->mode == RC_MODE_TEST) ? "test" : "operational",
			       (unsigned)s->step, names[i].name);
		}
	}
}

static void write_all(int fd, const uint8_t *buf, size_t n)
{
	while (n > 0U) {
		ssize_t w = write(fd, buf, n);

		if (w < 0) {
			if (errno == EAGAIN) {
				continue;
			}
			die("serial write");
		}
		buf += w;
		n -= (size_t)w;
	}
}

static void send_outputs(struct daemon *d, uint8_t mask)
{
	struct rc_set_outputs so = {.mask = mask};
	uint8_t out[RC_FRAME_MAX];
	size_t n = rc_encode_set_outputs(&so, out, sizeof(out));

	write_all(d->serial, out, n);
}

static void send_run_bit(struct daemon *d)
{
	uint8_t out[RC_FRAME_MAX];
	size_t n = rc_encode_run_bit(out, sizeof(out));

	write_all(d->serial, out, n);
}

/*
 * Periodic checks close the measurement windows and update health. An
 * on-demand check (RUN_BIT) only reads them, so it can neither shorten a
 * window nor speed up recovery.
 */
static void run_bit(struct daemon *d, int64_t t, bool periodic)
{
	struct bit_report prev = d->cbit;
	struct bit_input in;
	uint32_t errors = d->parser.crc_errors + d->parser.len_errors;
	long v = 0;

	if (periodic) {
		in.status_age_ms = role_take_status_age(&d->s, t);
	} else {
		in.status_age_ms = t - d->s.last_status_ms;
		if (d->s.max_status_gap_ms > in.status_age_ms) {
			in.status_age_ms = d->s.max_status_gap_ms;
		}
	}
	in.link_errors = errors - d->errors_at_check;
	in.max_hb_interval_ms = d->max_hb_interval_ms;
	in.peer_age_ms = d->s.peer_seen ? (t - d->s.last_peer_ms) : INT64_MAX;
	in.undervoltage_ok = (d->uv_path[0] != '\0') && read_long(d->uv_path, &v);
	in.undervoltage = in.undervoltage_ok && (v != 0);
	in.temp_ok = read_long(TEMP_PATH, &v);
	in.temp_mc = in.temp_ok ? (int32_t)v : 0;
	if (periodic) {
		d->errors_at_check = errors;
		d->max_hb_interval_ms = 0;
	}

	bit_evaluate(&in, t, &d->cbit);
	if (periodic && !d->health.pbit_done) {
		d->pbit = d->cbit;
		printf("t=%lld event=pbit result=%s\n", (long long)t,
		       bit_critical_pass(&d->pbit) ? "pass" : "fail");
	}
	for (int i = 0; i < BIT_ITEMS; i++) {
		if (!prev.valid || (prev.result[i] != d->cbit.result[i])) {
			char value[64];

			bit_item_value(&d->cbit, i, value, sizeof(value));
			printf("t=%lld event=bit item=%s result=%s value=\"%s\"\n", (long long)t,
			       bit_item_name(i), bit_result_name(d->cbit.result[i]), value);
		}
	}
	if (!periodic) {
		return;
	}
	if (bit_health_update(&d->health, &d->cbit) || !prev.valid) {
		printf("t=%lld slot=%c event=%s\n", (long long)t, slot_name(d->s.slot),
		       d->health.healthy ? "healthy" : "unhealthy");
	}
}

static void fill_view(const struct daemon *d, int64_t t, struct cmd_view *v)
{
	v->slot = d->s.slot;
	v->role = d->s.role;
	v->mode = d->s.mode;
	v->active_slot = d->s.active_slot;
	v->healthy = d->health.healthy;
	v->referee_ok = d->s.referee_ok;
	v->peer_ok = d->s.peer_ok;
	v->peer_healthy = d->s.peer_healthy;
	v->fault = d->s.fault;
	v->step = d->s.step;
	v->mask = role_current_mask(&d->s);
	v->io_fail = d->s.io_fail;
	v->pbit = &d->pbit;
	v->cbit = &d->cbit;
	v->now_ms = t;
}

static size_t handle_line(struct daemon *d, const char *line, int64_t t, char *out)
{
	struct cmd_view v;
	struct cmd_result res;
	size_t len;

	fill_view(d, t, &v);
	len = cmd_handle(line, &v, &res, out, CMD_REPLY_MAX);
	switch (res.action) {
	case CMD_ACT_SET_LEDS:
		role_set_test_mask(&d->s, res.leds, t);
		break;
	case CMD_ACT_LAMP_TEST:
		role_start_lamp_test(&d->s, t);
		break;
	case CMD_ACT_RUN_BIT:
		if ((t - d->last_run_bit_ms) >= RUN_BIT_MIN_INTERVAL_MS) {
			d->last_run_bit_ms = t;
			run_bit(d, t, false);
			send_run_bit(d);
		}
		break;
	case CMD_ACT_NONE:
		break;
	}
	if (res.action != CMD_ACT_NONE) {
		printf("t=%lld event=tcp_command line=\"%s\"\n", (long long)t, line);
	}
	return len;
}

static void client_close(struct client *c, int64_t t, const char *why)
{
	printf("t=%lld event=tcp_disconnect reason=\"%s\"\n", (long long)t, why);
	close(c->fd);
	c->fd = -1;
}

static bool client_send(struct client *c, const char *buf, size_t n)
{
	ssize_t w = send(c->fd, buf, n, MSG_NOSIGNAL);

	return (w >= 0) && ((size_t)w == n);
}

static void on_accept(struct daemon *d, int64_t t)
{
	int fd = accept4(d->listener, NULL, NULL, SOCK_NONBLOCK);

	if (fd < 0) {
		if ((errno != EAGAIN) && (errno != EINTR)) {
			printf("t=%lld event=tcp_accept_failed error=\"%s\"\n", (long long)t,
			       strerror(errno));
		}
		return;
	}
	for (int i = 0; i < TCP_CLIENTS; i++) {
		if (d->clients[i].fd < 0) {
			d->clients[i].fd = fd;
			cmd_linebuf_init(&d->clients[i].lb);
			printf("t=%lld event=tcp_connect\n", (long long)t);
			return;
		}
	}
	printf("t=%lld event=tcp_rejected reason=full\n", (long long)t);
	close(fd);
}

/* A misbehaving client is dropped; it never blocks the control loop. */
static void on_client(struct daemon *d, struct client *c, int64_t t)
{
	char buf[256];
	ssize_t n = recv(c->fd, buf, sizeof(buf), 0);

	if (n == 0) {
		client_close(c, t, "closed");
		return;
	}
	if (n < 0) {
		if (errno != EAGAIN) {
			client_close(c, t, strerror(errno));
		}
		return;
	}
	for (ssize_t i = 0; i < n; i++) {
		char line[CMD_LINE_MAX + 1U];
		char out[CMD_REPLY_MAX];
		enum cmd_feed r = cmd_linebuf_feed(&c->lb, buf[i], line);
		size_t len;

		if (r == CMD_FEED_NONE) {
			continue;
		}
		if (r == CMD_FEED_TOO_LONG) {
			len = cmd_too_long(out, sizeof(out));
			/* The client is dropped either way; a failed send changes nothing. */
			(void)client_send(c, out, len);
			client_close(c, t, "line too long");
			return;
		}
		len = handle_line(d, line, t, out);
		if (!client_send(c, out, len)) {
			client_close(c, t, "send failed");
			return;
		}
	}
}

static unsigned on_serial(struct daemon *d, int64_t t)
{
	uint8_t buf[64];
	ssize_t n = read(d->serial, buf, sizeof(buf));
	unsigned ev = 0U;

	if ((n < 0) && (errno != EAGAIN)) {
		die("serial read");
	}
	for (ssize_t i = 0; i < n; i++) {
		struct rc_frame f;
		struct rc_status st;

		if (!rc_parser_feed(&d->parser, buf[i], &f)) {
			continue;
		}
		do {
			if (rc_decode_status(&f, &st) == 0) {
				ev |= role_on_status(&d->s, &st, t);
			}
		} while (rc_parser_next(&d->parser, &f));
	}
	return ev;
}

static unsigned on_peer(struct daemon *d, int64_t t)
{
	uint8_t buf[RC_FRAME_MAX];
	ssize_t n = recv(d->peer, buf, sizeof(buf), 0);
	struct rc_parser pp;
	unsigned ev = 0U;

	/* The cross-link is monitoring only: a receive failure never stops control. */
	if ((n < 0) && (errno != EAGAIN)) {
		if (d->peer_recv_ok) {
			printf("t=%lld event=peer_recv_failed error=%s\n", (long long)t,
			       strerror(errno));
			d->peer_recv_ok = false;
		}
	} else if ((n >= 0) && !d->peer_recv_ok) {
		printf("t=%lld event=peer_recv_ok\n", (long long)t);
		d->peer_recv_ok = true;
	}
	rc_parser_init(&pp);
	for (ssize_t i = 0; i < n; i++) {
		struct rc_frame f;
		struct rc_peer pm;

		if (!rc_parser_feed(&pp, buf[i], &f)) {
			continue;
		}
		do {
			if (rc_decode_peer(&f, &pm) == 0) {
				ev |= role_on_peer(&d->s, &pm, t);
			}
		} while (rc_parser_next(&pp, &f));
	}
	return ev;
}

static void send_heartbeat(struct daemon *d, int64_t t)
{
	uint8_t healthy = d->health.healthy ? 1U : 0U;
	struct rc_heartbeat hb = {.seq = d->hb_seq++, .role = d->s.role, .healthy = healthy};
	struct rc_peer pm = {.seq = d->peer_seq++, .slot = d->s.slot, .role = d->s.role,
			     .referee_ok = d->s.referee_ok ? 1U : 0U, .step = d->s.step,
			     .healthy = healthy, .test_mask = d->s.test_mask};
	uint8_t out[RC_FRAME_MAX];
	size_t n = rc_encode_heartbeat(&hb, out, sizeof(out));

	if ((t - d->last_hb_ms) > d->max_hb_interval_ms) {
		d->max_hb_interval_ms = t - d->last_hb_ms;
	}
	d->last_hb_ms = t;
	write_all(d->serial, out, n);
	n = rc_encode_peer(&pm, out, sizeof(out));
	/* The cross-link is monitoring only: a send failure never stops control. */
	if (d->peer < 0) {
		return;
	}
	if (sendto(d->peer, out, n, 0, (struct sockaddr *)&d->group, sizeof(d->group)) < 0) {
		if ((errno != EAGAIN) && d->peer_send_ok) {
			printf("t=%lld event=peer_send_failed error=%s\n", (long long)t,
			       strerror(errno));
			d->peer_send_ok = false;
		}
	} else if (!d->peer_send_ok) {
		printf("t=%lld event=peer_send_ok\n", (long long)t);
		d->peer_send_ok = true;
	}
}

int main(void)
{
	static struct daemon d;
	int64_t start;
	int64_t next_hb;
	int64_t next_bit;

	setvbuf(stdout, NULL, _IOLBF, 0);
	d.serial = open_serial();
	d.peer = open_peer(&d.group);
	d.listener = open_listener();
	for (int i = 0; i < TCP_CLIENTS; i++) {
		d.clients[i].fd = -1;
	}
	if (find_undervoltage_path(d.uv_path, sizeof(d.uv_path)) != 0) {
		fprintf(stderr, "error: rpi_volt hwmon not found; supply_voltage reports error\n");
	}
	d.peer_send_ok = true;
	d.peer_recv_ok = true;
	rc_parser_init(&d.parser);
	start = now_ms();
	role_init(&d.s, start);
	bit_health_init(&d.health);
	d.last_hb_ms = start;
	next_hb = start;
	next_bit = start + BIT_PBIT_DELAY_MS;
	printf("t=%lld rc-central started\n", (long long)start);

	for (;;) {
		struct pollfd fds[3 + TCP_CLIENTS];
		int64_t wait = next_hb - now_ms();
		int64_t t;
		unsigned ev = 0U;
		uint8_t mask;

		fds[0] = (struct pollfd){.fd = d.serial, .events = POLLIN};
		fds[1] = (struct pollfd){.fd = d.peer, .events = POLLIN};
		fds[2] = (struct pollfd){.fd = d.listener, .events = POLLIN};
		for (int i = 0; i < TCP_CLIENTS; i++) {
			fds[3 + i] = (struct pollfd){.fd = d.clients[i].fd, .events = POLLIN};
		}
		if (poll(fds, 3 + TCP_CLIENTS, (wait > 0) ? (int)wait : 0) < 0) {
			if (errno == EINTR) {
				continue;
			}
			die("poll");
		}
		t = now_ms();
		if ((fds[0].revents & POLLIN) != 0) {
			ev |= on_serial(&d, t);
		}
		if ((fds[1].revents & POLLIN) != 0) {
			ev |= on_peer(&d, t);
		}
		ev |= role_tick(&d.s, t);
		log_events(ev, &d.s, t);

		if (t >= next_bit) {
			run_bit(&d, t, true);
			next_bit += BIT_PERIOD_MS;
			if (next_bit <= t) {
				next_bit = t + BIT_PERIOD_MS;
			}
		}
		if (t >= next_hb) {
			send_heartbeat(&d, t);
			next_hb += HEARTBEAT_PERIOD_MS;
			if (next_hb <= t) {
				next_hb = t + HEARTBEAT_PERIOD_MS;
			}
		}
		if ((fds[2].revents & POLLIN) != 0) {
			on_accept(&d, t);
		}
		for (int i = 0; i < TCP_CLIENTS; i++) {
			if ((d.clients[i].fd >= 0) &&
			    ((fds[3 + i].revents & (POLLIN | POLLHUP | POLLERR)) != 0)) {
				on_client(&d, &d.clients[i], t);
			}
		}
		if (role_chaser_due(&d.s, t, &mask) || role_test_output_due(&d.s, t, &mask)) {
			send_outputs(&d, mask);
		}
	}
	return 0;
}
