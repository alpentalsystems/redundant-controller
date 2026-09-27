#include <string.h>

#include "check.h"
#include "rc/proto.h"

static void test_crc16_check_value(void)
{
	const char *s = "123456789";

	CHECK(rc_crc16((const uint8_t *)s, strlen(s)) == 0x29B1U);
}

static void test_crc16_empty(void)
{
	CHECK(rc_crc16(NULL, 0U) == 0xFFFFU);
}

int main(void)
{
	test_crc16_check_value();
	test_crc16_empty();
	return CHECK_DONE();
}
