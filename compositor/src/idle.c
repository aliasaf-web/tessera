#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/util/log.h>
#include "server.h"

/* Idle handling: after a while without input the screen locks, a bit
 * later it turns off. Video players and the like can prevent this. */

static bool idle_inhibited(struct ts_server *server) {
	return !wl_list_empty(&server->idle_inhibit->inhibitors);
}

static void schedule(struct ts_server *server, int minutes) {
	if (minutes <= 0) {
		wl_event_source_timer_update(server->idle_timer, 0);
		return;
	}
	wl_event_source_timer_update(server->idle_timer, minutes * 60 * 1000);
}

static int idle_fire(void *data) {
	struct ts_server *server = data;
	struct ts_config *c = &server->config;
	if (idle_inhibited(server)) {
		wl_event_source_timer_update(server->idle_timer, 60 * 1000);
		return 0;
	}
	if (server->idle_stage == 0) {
		server->idle_stage = 1;
		if (c->idle_lock_minutes > 0) {
			lock_engage(server);
		}
		int rest = c->idle_screen_off_minutes - (c->idle_lock_minutes > 0 ? c->idle_lock_minutes : 0);
		if (c->idle_screen_off_minutes > 0) {
			if (rest <= 0) {
				output_set_dpms(server, false);
				server->idle_stage = 2;
			} else {
				schedule(server, rest);
			}
		}
	} else if (server->idle_stage == 1) {
		server->idle_stage = 2;
		output_set_dpms(server, false);
	}
	return 0;
}

void idle_reset(struct ts_server *server) {
	if (server->idle_timer == NULL) {
		return;
	}
	if (server->idle_stage == 2) {
		output_set_dpms(server, true);
	}
	server->idle_stage = 0;
	struct ts_config *c = &server->config;
	int first = c->idle_lock_minutes > 0 ? c->idle_lock_minutes : c->idle_screen_off_minutes;
	if (c->idle_lock_minutes > 0 && c->idle_screen_off_minutes > 0 &&
			c->idle_screen_off_minutes < c->idle_lock_minutes) {
		first = c->idle_screen_off_minutes;
	}
	schedule(server, first);
}

void idle_update_inhibit(struct ts_server *server) {
	wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, idle_inhibited(server));
}

struct inhibitor {
	struct ts_server *server;
	struct wl_listener destroy;
};

static void inhibitor_destroy(struct wl_listener *listener, void *data) {
	struct inhibitor *inh = wl_container_of(listener, inh, destroy);
	struct ts_server *server = inh->server;
	wl_list_remove(&inh->destroy.link);
	free(inh);
	/* The inhibitor is removed from the list after this signal. */
	wlr_idle_notifier_v1_set_inhibited(server->idle_notifier,
		wl_list_length(&server->idle_inhibit->inhibitors) > 1);
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
