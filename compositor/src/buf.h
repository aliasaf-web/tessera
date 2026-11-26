/* Appending text to a fixed buffer without walking off the end.
 *
 * snprintf returns how long the text *would* have been, not how much it
 * wrote. Adding that to a running length in a loop is a quiet way to write
 * past the buffer on the next round, so a builder that grows one line out of
 * many pieces uses these instead. */
#ifndef TESSERA_BUF_H
#define TESSERA_BUF_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Appends to buf (size bytes in all, len bytes of text in it so far; pass 0
 * to start, or the value returned last time) and returns the new length.
 * Text that does not all fit is left out, the buffer keeps its NUL at the
 * end, and once it is full nothing more is written. A piece is never cut in
 * half by mistake: use buf_fits below to leave room for the whole thing. */
static inline size_t buf_append(char *buf, size_t size, size_t len, const char *fmt, ...) {
	if (size == 0) {
		return 0;
	}
	if (len >= size - 1) { /* full already, or a length that cannot be right */
		buf[size - 1] = '\0';
		return size - 1;
	}
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf + len, size - len, fmt, ap);
	va_end(ap);
	if (n < 0) {
		return len; /* the format itself failed: leave the text as it was */
	}
	size_t room = size - len - 1; /* what fits before the NUL */
	return len + ((size_t)n < room ? (size_t)n : room);
}

/* True when need more bytes still fit after len, with room to spare for a
 * NUL. Checking before appending means a list can stop at a whole item
 * instead of ending in half of one. */
static inline bool buf_fits(size_t size, size_t len, size_t need) {
	return size > 0 && len <= size - 1 && need <= size - 1 - len;
}

#endif
