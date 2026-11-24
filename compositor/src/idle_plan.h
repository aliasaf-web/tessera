#ifndef TESSERA_IDLE_PLAN_H
#define TESSERA_IDLE_PLAN_H

/* When the screen locks and turns off, counted in minutes without input
 * (0 = never). If locking is on, the screen also locks when it turns off,
 * so a dark screen is never left unlocked. */
struct idle_plan {
	int lock_at;
	int off_at;
};

static inline struct idle_plan idle_plan_make(int lock_minutes, int off_minutes) {
	struct idle_plan plan = {
		.lock_at = lock_minutes > 0 ? lock_minutes : 0,
		.off_at = off_minutes > 0 ? off_minutes : 0,
	};
	if (plan.lock_at > 0 && plan.off_at > 0 && plan.off_at < plan.lock_at) {
		plan.lock_at = plan.off_at;
	}
	return plan;
}

/* The next moment after `elapsed` minutes when something happens, or 0 if
 * nothing is left to do. */
static inline int idle_plan_next(struct idle_plan plan, int elapsed) {
	int next = 0;
	if (plan.lock_at > elapsed) {
		next = plan.lock_at;
	}
	if (plan.off_at > elapsed && (next == 0 || plan.off_at < next)) {
		next = plan.off_at;
	}
	return next;
}

#endif
