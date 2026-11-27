#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "proc.h"

/* A process cannot choose who forked it, so the parent is worth reading when
 * the question is "did the desktop ask for this, or a program in the session".
 * getppid() is 1 when the parent has already gone, which reads as a different
 * executable and is refused - the right answer, since there is then no desktop
 * waiting for the result. */
bool proc_parent_is_self(void) {
	char self[4096], parent[4096];
	ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (n <= 0) {
		return false;
	}
	self[n] = '\0';
	char link[64];
	snprintf(link, sizeof(link), "/proc/%d/exe", (int)getppid());
	ssize_t m = readlink(link, parent, sizeof(parent) - 1);
	if (m <= 0) {
		return false;
	}
	parent[m] = '\0';
	return strcmp(self, parent) == 0;
}
