/* Growing one line out of many pieces inside a fixed buffer: nothing ever
 * lands outside it, a piece that doesn't fit is left out whole, and the text
 * that does fit is exact. The state and switcher lines are built this way. */
#include <stdio.h>
#include <string.h>
#include "../src/buf.h"

static int failures;

static void fail(const char *what, const char *got) {
	printf("FAIL: %s (got \"%s\")\n", what, got);
	failures++;
}

/* Writes into the middle of a guarded array and checks the guard bytes on
 * both sides afterwards: a write past the end shows up here. */
#define CAP 24
#define GUARD 8

static char block[GUARD + CAP + GUARD];

static void start(void) {
	memset(block, '#', sizeof(block));
	memset(block + GUARD, 0, CAP);
}

static char *text(void) {
	return block + GUARD;
}

static void check_guard(const char *what) {
	for (size_t i = 0; i < GUARD; i++) {
		if (block[i] != '#' || block[GUARD + CAP + i] != '#') {
			printf("FAIL: %s wrote outside the buffer\n", what);
			failures++;
			return;
		}
	}
}

/* Builds "1,2,3,..." of the first `count` numbers into buf (size bytes),
 * leaving out a number that would not fit whole. Returns the length. */
static size_t build_list(char *buf, size_t size, int count) {
	size_t len = 0;
	for (int i = 1; i <= count; i++) {
		char item[8];
		size_t n = buf_append(item, sizeof(item), 0, "%s%d", len ? "," : "", i);
		if (!buf_fits(size, len, n)) {
			break;
		}
		len = buf_append(buf, size, len, "%s", item);
	}
	return len;
}

int main(void) {
	/* Plain appends: the text is exactly what was asked for. */
	start();
	size_t len = buf_append(text(), CAP, 0, "%s", "workspace");
	len = buf_append(text(), CAP, len, " %d", 3);
	len = buf_append(text(), CAP, len, " %s", "on HDMI-A-1");
	check_guard("appending");
	if (strcmp(text(), "workspace 3 on HDMI-A-1") != 0) {
		fail("three appends make one line", text());
	}

	/* A full buffer keeps what it had, and its NUL stays put. */
	len = buf_append(text(), CAP, len, "%s", " and more that cannot fit");
	check_guard("appending past the end");
	if (strcmp(text(), "workspace 3 on HDMI-A-1") != 0) {
		fail("text that does not fit is left out", text());
	}
	if (len != CAP - 1) {
		printf("FAIL: a full buffer reported length %zu, wanted %d\n", len, CAP - 1);
		failures++;
	}
	/* Asking again after it is full changes nothing (and cannot crash). */
	size_t again = buf_append(text(), CAP, len, "%s", "x");
	check_guard("appending to a full buffer");
	if (again != len || strlen(text()) != CAP - 1) {
		fail("a full buffer stays as it was", text());
	}

	/* Exact fit: filling the buffer to the last byte still gives the whole
	 * text, because the NUL was counted. */
	char small[8];
	size_t n = buf_append(small, sizeof(small), 0, "%s", "abcdefg");
	if (n != 7 || strcmp(small, "abcdefg") != 0) {
		printf("FAIL: exact fit gave %zu bytes (\"%s\"), wanted 7 (\"abcdefg\")\n", n, small);
		failures++;
	}

	/* buf_fits agrees with what appending actually does: whatever it says
	 * fits, fits whole. */
	for (size_t size = 1; size <= 20; size++) {
		for (size_t have = 0; have < size; have++) {
			for (size_t need = 0; need <= 20; need++) {
				char buf[32];
				memset(buf, '#', sizeof(buf));
				buf[0] = 'a';
				bool ok = buf_fits(size, have, need);
				char filler[24];
				memset(filler, 'b', sizeof(filler));
				filler[need] = '\0';
				size_t after = buf_append(buf, size, have, "%s", filler);
				bool whole = after == have + need;
				if (ok != whole) {
					printf("FAIL: buf_fits(%zu, %zu, %zu) said %d, appending gave %zu\n",
						size, have, need, ok, after);
					failures++;
				}
				for (size_t i = size; i < sizeof(buf); i++) {
					if (buf[i] != '#') {
						printf("FAIL: buf_append(%zu, %zu, %zu) wrote past %zu bytes\n",
							size, have, need, size);
						failures++;
						break;
					}
				}
			}
		}
	}

	/* The list case from the control socket: workspace numbers, comma
	 * separated. Sized as the code sizes them (four bytes each: up to three
	 * digits and a comma), the list comes out whole for any count. */
	for (int count = 1; count <= 60; count++) {
		size_t size = (size_t)count * 4 + 1;
		char list[256], want[256];
		if (size > sizeof(list)) {
			break;
		}
		size_t len2 = build_list(list, size, count);
		size_t wanted = build_list(want, sizeof(want), count);
		if (len2 != wanted || strcmp(list, want) != 0) {
			printf("FAIL: %d workspaces in %zu bytes gave \"%s\", wanted \"%s\"\n",
				count, size, list, want);
			failures++;
		}
	}

	/* Too little room: the list stops at a whole number - never "1,2,3,"
	 * with the four cut off - at every buffer size, and the part it kept is
	 * the longest that fits. */
	for (size_t size = 1; size <= 40; size++) {
		char list[40];
		memset(list, 0, sizeof(list)); /* every size ends with a NUL to stop strlen */
		size_t len2 = build_list(list, size, 9);
		if (len2 > size - 1) {
			printf("FAIL: %zu bytes reported a length of %zu\n", size, len2);
			failures++;
		}
		size_t digits = strspn(list, "0123456789,");
		if (digits != len2 || digits != strlen(list) ||
				(digits > 0 && list[digits - 1] == ',')) {
			printf("FAIL: %zu bytes gave a cut-off list (\"%s\")\n", size, list);
			failures++;
		}
		/* One more number would have fitted if there were a byte more. */
		char bigger[40];
		memset(bigger, 0, sizeof(bigger));
		if (size + 1 <= sizeof(bigger) && build_list(bigger, size + 1, 9) < len2) {
			printf("FAIL: %zu bytes held more than %zu bytes did\n", size + 1, size);
			failures++;
		}
	}

	if (failures == 0) {
		printf("all appends stayed inside the buffer\n");
	}
	return failures ? 1 : 0;
}
