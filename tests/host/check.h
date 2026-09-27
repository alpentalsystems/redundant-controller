#ifndef CHECK_H_
#define CHECK_H_

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                            \
		}                                                              \
	} while (0)

#define CHECK_DONE()                                                           \
	((failures == 0) ? (printf("all checks passed\n"), 0)                 \
			 : (printf("%d check(s) failed\n", failures), 1))

#endif /* CHECK_H_ */
