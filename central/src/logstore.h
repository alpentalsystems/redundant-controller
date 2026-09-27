#ifndef LOGSTORE_H_
#define LOGSTORE_H_

#include <stdint.h>

#include "rc/logrec.h"

/* 10 MiB of 64-byte records. */
#define LOGSTORE_SLOTS 163840U

struct logstore {
	int fd;
	uint32_t slots;
	uint32_t next_seq;
};

/*
 * Opens or creates the ring file, extends it to slots records, and
 * continues after the highest valid seq. Returns 0 or -errno.
 */
int logstore_open(struct logstore *ls, const char *path, uint32_t slots);

/* Sets rec->seq and writes the record to slot seq mod slots. Returns 0 or -errno. */
int logstore_append(struct logstore *ls, struct rc_log_record *rec);

int logstore_close(struct logstore *ls);

#endif /* LOGSTORE_H_ */
