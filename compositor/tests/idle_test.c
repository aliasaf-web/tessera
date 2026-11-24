/* Checks when the screen locks and turns off for each pair of settings,
 * by walking through the plan the way the idle timer does. */
#include <stdio.h>
#include "../src/idle_plan.h"

struct expect {
	int lock_minutes, off_minutes; /* the settings (0 = never) */
	int locks_at, turns_off_at;    /* minutes without input (0 = never) */
};

static const struct expect cases[] = {
	{ 0, 10, 0, 10 },   /* never lock: off after 10, not 20 */
	{ 5, 0, 5, 0 },     /* never turn off */
	{ 5, 10, 5, 10 },   /* lock first, then off */
	{ 10, 10, 10, 10 }, /* both at once */
	{ 10, 5, 5, 5 },    /* off first: it locks too */
	{ 0, 0, 0, 0 },     /* never anything */
	{ 1, 30, 1, 30 },
};

int main(void) {
	int failures = 0;
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const struct expect *e = &cases[i];
		struct idle_plan plan = idle_plan_make(e->lock_minutes, e->off_minutes);
		int locked = 0, off = 0, elapsed = 0, steps = 0;
		int next;
		while ((next = idle_plan_next(plan, elapsed)) > 0 && steps++ < 10) {
			elapsed = next;
			if (plan.lock_at > 0 && plan.lock_at <= elapsed && locked == 0) {
				locked = elapsed;
			}
			if (plan.off_at > 0 && plan.off_at <= elapsed && off == 0) {
				off = elapsed;
			}
		}
		if (locked != e->locks_at || off != e->turns_off_at) {
			printf("FAIL lock=%d off=%d: locked at %d (want %d), off at %d (want %d)\n",
				e->lock_minutes, e->off_minutes, locked, e->locks_at, off, e->turns_off_at);
			failures++;
		}
	}
	printf("%s\n", failures ? "some cases failed" : "all cases passed");
	return failures ? 1 : 0;
}
