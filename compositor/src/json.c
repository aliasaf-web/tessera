#include <stdio.h>
#include "json.h"

void json_escape(char *out, size_t size, const char *in) {
	size_t o = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
		if (o + 7 >= size) {
			/* Out of room: drop a UTF-8 character that was only partly copied. */
			size_t lead = o;
			while (lead > 0 && ((unsigned char)out[lead - 1] & 0xC0) == 0x80) {
				lead--;
			}
			if (lead > 0 && ((unsigned char)out[lead - 1] & 0x80)) {
				unsigned char first = (unsigned char)out[lead - 1];
				size_t length = first >= 0xF0 ? 4 : first >= 0xE0 ? 3 : 2;
				if (o - (lead - 1) < length) {
					o = lead - 1;
				}
			}
			break;
		}
		if (*p == '"' || *p == '\\') {
			out[o++] = '\\';
			out[o++] = *p;
		} else if (*p < 0x20) {
			o += snprintf(out + o, size - o, "\\u%04x", *p);
		} else {
			out[o++] = *p;
		}
	}
	out[o] = '\0';
}
