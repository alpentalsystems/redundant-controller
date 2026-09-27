#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "logstore.h"

#define SCAN_RECORDS 1024U

static int open_failed(struct logstore *ls)
{
	int err = -errno;

	close(ls->fd);
	ls->fd = -1;
	return err;
}

int logstore_open(struct logstore *ls, const char *path, uint32_t slots)
{
	static uint8_t buf[SCAN_RECORDS * RC_LOG_RECORD_SIZE];
	off_t size = (off_t)slots * RC_LOG_RECORD_SIZE;
	struct stat st;
	bool found = false;
	uint32_t max_seq = 0U;

	ls->fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (ls->fd < 0) {
		ls->fd = -1;
		return -errno;
	}
	if (fstat(ls->fd, &st) != 0) {
		return open_failed(ls);
	}
	if ((st.st_size < size) && (ftruncate(ls->fd, size) != 0)) {
		return open_failed(ls);
	}
	for (uint32_t first = 0U; first < slots; first += SCAN_RECORDS) {
		uint32_t n = ((slots - first) < SCAN_RECORDS) ? (slots - first) : SCAN_RECORDS;
		ssize_t got = pread(ls->fd, buf, (size_t)n * RC_LOG_RECORD_SIZE,
				    (off_t)first * RC_LOG_RECORD_SIZE);

		if (got < 0) {
			return open_failed(ls);
		}
		for (size_t i = 0U; ((i + 1U) * RC_LOG_RECORD_SIZE) <= (size_t)got; i++) {
			struct rc_log_record r;

			if ((rc_log_decode(&buf[i * RC_LOG_RECORD_SIZE], &r) == 0) &&
			    (!found || (r.seq > max_seq))) {
				max_seq = r.seq;
				found = true;
			}
		}
	}
	ls->slots = slots;
	ls->next_seq = found ? (max_seq + 1U) : 0U;
	return 0;
}

int logstore_append(struct logstore *ls, struct rc_log_record *rec)
{
	uint8_t buf[RC_LOG_RECORD_SIZE];
	off_t off = (off_t)(ls->next_seq % ls->slots) * RC_LOG_RECORD_SIZE;
	ssize_t w;

	/* The seq advances even if the write fails, so a loss shows as a gap. */
	rec->seq = ls->next_seq++;
	rc_log_encode(rec, buf);
	w = pwrite(ls->fd, buf, sizeof(buf), off);
	if (w < 0) {
		return -errno;
	}
	return ((size_t)w == sizeof(buf)) ? 0 : -EIO;
}

int logstore_close(struct logstore *ls)
{
	int r = close(ls->fd);

	ls->fd = -1;
	return (r == 0) ? 0 : -errno;
}
