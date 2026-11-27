#ifndef TESSERA_PROC_H
#define TESSERA_PROC_H

#include <stdbool.h>

/* True when this process was started by a program with the same executable -
 * which is what the desktop's own short-lived helpers look like, since it
 * forks and then execs /proc/self/exe. Used by the password helper to refuse
 * anyone else. */
bool proc_parent_is_self(void);

#endif
