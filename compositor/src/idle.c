#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/util/log.h>
#include "idle_plan.h"
#include "server.h"

/* Idle handling: after a while without input the screen locks, a bit
 * later it turns off. Video players and the like can prevent this. */

/* A window (a video player, say) may keep the screen on, but only while it
 * can be seen: not from a hidden workspace, from under another window, or
 * from behind the lock screen. `leaving` is an inhibitor about to go. */
static bool inhibited_except(struct ts_server *server, struct wlr_idle_inhibitor_v1 *leaving) {
	struct wlr_idle_inhibitor_v1 *inhibitor;
	wl_list_for_each(inhibitor, &server->idle_inhibit->inhibitors, link) {
		/* A surface is on a screen only where part of it is visible. */
		if (inhibitor != leaving && !wl_list_empty(&inhibitor->surface->current_outputs)) {
			return true;
		}
	}
	return false;
}

static bool idle_inhibited(struct ts_server *server) {
	return inhibited_except(server, NULL);
}

/* Waits until the next step of the plan, counted from the last input. */
static void schedule_next(struct ts_server *server) {
	struct ts_config *c = &server->config;
	struct idle_plan plan = idle_plan_make(c->idle_lock_minutes, c->idle_screen_off_minutes);
	int next = idle_plan_next(plan, server->idle_elapsed);
	server->idle_next = next;
	wl_event_source_timer_update(server->idle_timer,
		next > 0 ? (next - server->idle_elapsed) * 60 * 1000 : 0);
}

static int idle_fire(void *data) {
	struct ts_server *server = data;
	struct ts_config *c = &server->config;
	idle_update_inhibit(server);
	if (idle_inhibited(server)) {
		wl_event_source_timer_update(server->idle_timer, 60 * 1000);
		return 0;
	}
	server->idle_elapsed = server->idle_next;
	struct idle_plan plan = idle_plan_make(c->idle_lock_minutes, c->idle_screen_off_minutes);
	if (plan.lock_at > 0 && plan.lock_at <= server->idle_elapsed) {
		lock_engage(server);
	}
	if (plan.off_at > 0 && plan.off_at <= server->idle_elapsed && !server->idle_screen_off) {
		server->idle_screen_off = true;
		output_set_dpms(server, false);
	}
	schedule_next(server);
	return 0;
}

void idle_reset(struct ts_server *server) {
	if (server->idle_timer == NULL) {
		return;
	}
	if (server->idle_screen_off) {
		server->idle_screen_off = false;
		output_set_dpms(server, true);
	}
	server->idle_elapsed = 0;
	schedule_next(server);
}

/* Tells other programs that watch for idleness (ext-idle-notify); called
 * when windows are shown or hidden. */
void idle_update_inhibit(struct ts_server *server) {
	if (server->idle_inhibit != NULL) {
		wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, idle_inhibited(server));
	}
}

struct inhibitor {
	struct ts_server *server;
	struct wlr_idle_inhibitor_v1 *wlr_inhibitor;
	struct wl_listener destroy;
};

static void inhibitor_destroy(struct wl_listener *listener, void *data) {
	struct inhibitor *inh = wl_container_of(listener, inh, destroy);
	struct ts_server *server = inh->server;
	/* It is still in the list during this signal. */
	bool inhibited = inhibited_except(server, inh->wlr_inhibitor);
	wl_list_remove(&inh->destroy.link);
	free(inh);
	wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, inhibited);
	idle_reset(server);
}

static void new_inhibitor(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, new_inhibitor);
	struct wlr_idle_inhibitor_v1 *wlr_inh = data;
	struct inhibitor *inh = calloc(1, sizeof(*inh));
	if (inh == NULL) {
		wlr_log(WLR_ERROR, "Out of memory");
		return;
	}
	inh->server = server;
	inh->wlr_inhibitor = wlr_inh;
	inh->destroy.notify = inhibitor_destroy;
	wl_signal_add(&wlr_inh->events.destroy, &inh->destroy);
	idle_update_inhibit(server);
}

void idle_init(struct ts_server *server) {
	server->idle_inhibit = wlr_idle_inhibit_v1_create(server->display);
	server->new_inhibitor.notify = new_inhibitor;
	wl_signal_add(&server->idle_inhibit->events.new_inhibitor, &server->new_inhibitor);
	server->idle_timer = wl_event_loop_add_timer(server->loop, idle_fire, server);
	idle_reset(server);
}
