#include <stdio.h>
#include <string.h>

#include "rc/logrec.h"
#include "rc/proto.h"

/* Writes three known records for the Python codec test: logrec_sample FILE */
int main(int argc, char **argv)
{
	struct rc_log_record r[3];
	uint8_t buf[RC_LOG_RECORD_SIZE];
	FILE *f;

	if (argc != 2) {
		fprintf(stderr, "usage: logrec_sample FILE\n");
		return 2;
	}
	memset(r, 0, sizeof(r));
	r[0].type = RC_LOG_SNAPSHOT;
	r[0].seq = 7U;
	r[0].io_time_ms = 123456U;
	r[0].mono_ms = 9876543210ULL;
	r[0].wall_ms = 1790000000123ULL;
	r[0].slot = RC_SLOT_A;
	r[0].role = RC_ROLE_ACTIVE;
	r[0].mode = RC_MODE_TEST;
	r[0].flags = RC_LOG_F_HEALTHY | RC_LOG_F_REFEREE_OK | RC_LOG_F_IO_TIME_VALID;
	r[0].active_slot = RC_SLOT_A;
	r[0].io_fail = 0x01U;
	r[0].bit_results = 0x0350U;
	r[0].step = 513U;
	r[0].mask = 0x55U;
	r[0].io_boot_id = 0xBEEFU;
	r[1] = r[0];
	r[1].type = RC_LOG_EVENT;
	r[1].seq = 8U;
	r[1].event = RC_LOG_EV_ROLE_CHANGED;
	r[1].detail = RC_ROLE_STANDBY;
	r[2] = r[0];
	r[2].type = RC_LOG_EVENT;
	r[2].seq = 9U;
	r[2].event = RC_LOG_EV_BIT_ITEM;
	r[2].detail = (3U << 8) | 1U;
	f = fopen(argv[1], "wb");
	if (f == NULL) {
		perror(argv[1]);
		return 1;
	}
	for (int i = 0; i < 3; i++) {
		rc_log_encode(&r[i], buf);
		if (fwrite(buf, 1U, sizeof(buf), f) != sizeof(buf)) {
			perror("write");
			fclose(f);
			return 1;
		}
	}
	if (fclose(f) != 0) {
		perror("close");
		return 1;
	}
	return 0;
}
