#ifndef TESSERA_JSON_H
#define TESSERA_JSON_H

#include <stddef.h>

/* Copies text into out as the inside of a JSON string, cut short (between
 * characters) if out is too small. */
void json_escape(char *out, size_t size, const char *in);

#endif
