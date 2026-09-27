#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "logstore.h"

static char path[64];

/* A unique path whose file does not exist yet. */
static void fresh_path(void)
{
	int fd;

	snprintf(path, sizeof(path), "/tmp/rc-logstore-XXXXXX");
	fd = mkstemp(path);
	CHECK(fd >= 0);
	close(fd);
	unlink(path);
}

static struct rc_log_record snap(void)
{
	struct rc_log_record r;

	memset(&r, 0, sizeof(r));
	r.type = RC_LOG_SNAPSHOT;
	r.step = 7U;
	return r;
}

/* Seq stored in a slot, or -1 when the slot does not hold a valid record. */
static long seq_in_slot(uint32_t slot)
{
	uint8_t buf[RC_LOG_RECORD_SIZE];
	struct rc_log_record r;
	int fd = open(path, O_RDONLY);
	ssize_t n = pread(fd, buf, sizeof(buf), (off_t)slot * RC_LOG_RECORD_SIZE);

	close(fd);
	if ((n != (ssize_t)sizeof(buf)) || (rc_log_decode(buf, &r) != 0)) {
		return -1;
	}
	return (long)r.seq;
}

static void append_n(struct logstore *ls, int n)
{
	for (int i = 0; i < n; i++) {
		struct rc_log_record r = snap();

		CHECK(logstore_append(ls, &r) == 0);
	}
}

static void test_new_file(void)
{
	struct logstore ls;
	struct rc_log_record r = snap();
	struct stat st;

	fresh_path();
	CHECK(logstore_open(&ls, path, 8U) == 0);
	CHECK(stat(path, &st) == 0 && st.st_size == 8 * 64);
	CHECK(ls.next_seq == 0U);
	CHECK(logstore_append(&ls, &r) == 0);
	CHECK(r.seq == 0U);
	append_n(&ls, 2);
	CHECK(seq_in_slot(1U) == 1 && seq_in_slot(2U) == 2 && seq_in_slot(3U) == -1);
	CHECK(logstore_close(&ls) == 0);
	unlink(path);
}

static void test_wrap_and_resume(void)
{
	struct logstore ls;
	struct rc_log_record r = snap();

	fresh_path();
	CHECK(logstore_open(&ls, path, 4U) == 0);
	append_n(&ls, 6);
	CHECK(seq_in_slot(0U) == 4 && seq_in_slot(1U) == 5 && seq_in_slot(2U) == 2);
	CHECK(logstore_close(&ls) == 0);
	CHECK(logstore_open(&ls, path, 4U) == 0);
	CHECK(ls.next_seq == 6U);
	CHECK(logstore_append(&ls, &r) == 0);
	CHECK(r.seq == 6U && seq_in_slot(2U) == 6);
	CHECK(logstore_close(&ls) == 0);
	unlink(path);
}

static void test_corrupt_slot_ignored(void)
{
	struct logstore ls;
	uint8_t junk = 0xFFU;
	int fd;

	fresh_path();
	CHECK(logstore_open(&ls, path, 4U) == 0);
	append_n(&ls, 6); /* slots hold seq 4, 5, 2, 3 */
	CHECK(logstore_close(&ls) == 0);
	fd = open(path, O_WRONLY);
	CHECK(pwrite(fd, &junk, 1U, 64 + 20) == 1); /* damage seq 5 */
	close(fd);
	CHECK(logstore_open(&ls, path, 4U) == 0);
	CHECK(ls.next_seq == 5U);
	CHECK(logstore_close(&ls) == 0);
	unlink(path);
}

static void test_short_garbage_file_extended(void)
{
	struct logstore ls;
	struct stat st;
	uint8_t junk[100];
	int fd;

	fresh_path();
	memset(junk, 0xA5, sizeof(junk));
	fd = open(path, O_WRONLY | O_CREAT, 0644);
	CHECK(write(fd, junk, sizeof(junk)) == (ssize_t)sizeof(junk));
	close(fd);
	CHECK(logstore_open(&ls, path, 4U) == 0);
	CHECK(stat(path, &st) == 0 && st.st_size == 4 * 64);
	CHECK(ls.next_seq == 0U);
	CHECK(logstore_close(&ls) == 0);
	unlink(path);
}

static void test_open_error_reported(void)
{
	struct logstore ls;

	CHECK(logstore_open(&ls, "/nonexistent-rc-dir/rc-log.bin", 4U) == -ENOENT);
	CHECK(ls.fd == -1);
}

int main(void)
{
	test_new_file();
	test_wrap_and_resume();
	test_corrupt_slot_ignored();
	test_short_garbage_file_extended();
	test_open_error_reported();
	return CHECK_DONE();
}
