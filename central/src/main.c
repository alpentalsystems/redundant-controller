#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "rc/proto.h"
#include "role.h"

#define SERIAL_DEV "/dev/serial0"
#define PEER_IFACE "eth0"
#define PEER_GROUP "ff02::1"
#define PEER_PORT 47000
#define HEARTBEAT_PERIOD_MS 20

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
	};

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if ((ev & names[i].bit) != 0U) {
			printf("t=%lld slot=%c role=%s step=%u event=%s\n", (long long)t,
			       slot_name(s->slot), role_name(s->role), (unsigned)s->step,
			       names[i].name);
		}
	}
	if (ev != 0U) {
		fflush(stdout);
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

int main(void)
{
	struct sockaddr_in6 group;
	int serial = open_serial();
	int peer = open_peer(&group);
	struct rc_parser parser;
	struct role_state s;
	uint32_t hb_seq = 0U;
	uint32_t peer_seq = 0U;
	int64_t next_hb = now_ms();
	bool peer_send_ok = true;
	bool peer_recv_ok = true;

	setvbuf(stdout, NULL, _IOLBF, 0);
	rc_parser_init(&parser);
	role_init(&s, now_ms());
	printf("t=%lld rc-central started\n", (long long)now_ms());

	for (;;) {
		struct pollfd fds[2] = {{.fd = serial, .events = POLLIN}, {.fd = peer, .events = POLLIN}};
		int64_t wait = next_hb - now_ms();
		int64_t t;
		unsigned ev = 0U;
		uint8_t mask;

		if (poll(fds, 2, (wait > 0) ? (int)wait : 0) < 0) {
			if (errno == EINTR) {
				continue;
			}
			die("poll");
		}
		t = now_ms();
		if ((fds[0].revents & POLLIN) != 0) {
			uint8_t buf[64];
			ssize_t n = read(serial, buf, sizeof(buf));

			if ((n < 0) && (errno != EAGAIN)) {
				die("serial read");
			}
			for (ssize_t i = 0; i < n; i++) {
				struct rc_frame f;
				struct rc_status st;

				if (!rc_parser_feed(&parser, buf[i], &f)) {
					continue;
				}
				do {
					if (rc_decode_status(&f, &st) == 0) {
						ev |= role_on_status(&s, &st, t);
					}
				} while (rc_parser_next(&parser, &f));
			}
		}
		if ((fds[1].revents & POLLIN) != 0) {
			uint8_t buf[RC_FRAME_MAX];
			ssize_t n = recv(peer, buf, sizeof(buf), 0);
			struct rc_parser pp;

			/* The cross-link is monitoring only: a receive failure never stops control. */
			if ((n < 0) && (errno != EAGAIN)) {
				if (peer_recv_ok) {
					printf("t=%lld event=peer_recv_failed error=%s\n", (long long)t,
					       strerror(errno));
					peer_recv_ok = false;
				}
			} else if ((n >= 0) && !peer_recv_ok) {
				printf("t=%lld event=peer_recv_ok\n", (long long)t);
				peer_recv_ok = true;
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
						ev |= role_on_peer(&s, &pm, t);
					}
				} while (rc_parser_next(&pp, &f));
			}
		}
		ev |= role_tick(&s, t);
		log_events(ev, &s, t);

		if (t >= next_hb) {
			struct rc_heartbeat hb = {.seq = hb_seq++, .role = s.role};
			struct rc_peer pm = {.seq = peer_seq++, .slot = s.slot, .role = s.role,
					     .referee_ok = s.referee_ok ? 1U : 0U, .step = s.step};
			uint8_t out[RC_FRAME_MAX];
			size_t n = rc_encode_heartbeat(&hb, out, sizeof(out));

			write_all(serial, out, n);
			n = rc_encode_peer(&pm, out, sizeof(out));
			/* The cross-link is monitoring only: a send failure never stops control. */
			if (peer >= 0) {
				if (sendto(peer, out, n, 0, (struct sockaddr *)&group, sizeof(group)) < 0) {
					if ((errno != EAGAIN) && peer_send_ok) {
						printf("t=%lld event=peer_send_failed error=%s\n",
						       (long long)t, strerror(errno));
						peer_send_ok = false;
					}
				} else if (!peer_send_ok) {
					printf("t=%lld event=peer_send_ok\n", (long long)t);
					peer_send_ok = true;
				}
			}
			next_hb += HEARTBEAT_PERIOD_MS;
			if (next_hb <= t) {
				next_hb = t + HEARTBEAT_PERIOD_MS;
			}
		}
		if (role_chaser_due(&s, t, &mask)) {
			struct rc_set_outputs so = {.mask = mask};
			uint8_t out[RC_FRAME_MAX];
			size_t n = rc_encode_set_outputs(&so, out, sizeof(out));

			write_all(serial, out, n);
		}
	}
	return 0;
}
