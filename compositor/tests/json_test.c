/* JSON escaping: special characters, and cutting text short without
 * breaking a character in two. */
#include <stdio.h>
#include <string.h>
#include "../src/json.h"

static int failures;

static void expect(const char *in, size_t size, const char *want) {
	char out[256];
	json_escape(out, size, in);
	if (strcmp(out, want) != 0) {
		printf("FAIL: \"%s\" in %zu bytes gave \"%s\", wanted \"%s\"\n", in, size, out, want);
		failures++;
	}
}

int main(void) {
	expect("plain", 64, "plain");
	expect("say \"hi\"", 64, "say \\\"hi\\\"");
	expect("back\\slash", 64, "back\\\\slash");
	expect("tab\there\n", 64, "tab\\u0009here\\u000a");
	expect("caf\xc3\xa9", 64, "caf\xc3\xa9");
	/* json_escape keeps 7 bytes spare, so size 11 leaves room for 4 bytes of
	 * text. Here the é would be cut in half, so it goes. */
	expect("abc\xc3\xa9xyz", 11, "abc");
	/* A whole é fits: it stays. */
	expect("ab\xc3\xa9xyz", 11, "ab\xc3\xa9");
	/* Three-byte and four-byte characters. */
	expect("a\xe2\x82\xac\xe2\x82\xac", 12, "a\xe2\x82\xac");
	expect("\xf0\x9f\x98\x80z", 10, "");
	if (failures == 0) {
		printf("all text escaped correctly\n");
	}
	return failures ? 1 : 0;
}
