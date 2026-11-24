#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "server.h"

/* Alt+Tab (and Super+Tab). The compositor keeps track of the choice and
 * the keys; the shell only draws the list it is sent. */

#define SWITCHER_MAX (int)(sizeof(((struct ts_server *)0)->switcher_ids) / sizeof(uint32_t))
#define TITLE_LEN 400
#define APP_ID_LEN 160
/* Room for one window in the message: its title and app id plus the rest. */
#define ITEM_LEN (TITLE_LEN + APP_ID_LEN + 96)

static void switcher_send(struct ts_server *server) {
	if (!server->switcher_active) {
		ipc_broadcast(server, "{\"event\":\"switcher\",\"visible\":false}");
		return;
	}
	size_t size = 96 + (size_t)server->switcher_count * ITEM_LEN;
	char *buf = malloc(size);
	if (buf == NULL) {
		return;
	}
	size_t o = snprintf(buf, size, "{\"event\":\"switcher\",\"visible\":true,\"selected\":%d,"
		"\"windows\":[", server->switcher_sel);
	bool first = true;
	for (int i = 0; i < server->switcher_count && o < size; i++) {
		struct ts_view *view = view_by_id(server, server->switcher_ids[i]);
		char title[TITLE_LEN], app[APP_ID_LEN];
		ipc_json_escape(title, sizeof(title), view ? view_title(view) : "");
		ipc_json_escape(app, sizeof(app), view ? view_app_id(view) : "");
		o += snprintf(buf + o, size - o,
			"%s{\"id\":%u,\"title\":\"%s\",\"app_id\":\"%s\",\"workspace\":%d}",
			first ? "" : ",", server->switcher_ids[i], title, app,
			view ? view->workspace : 0);
		first = false;
	}
	if (o < size) {
		snprintf(buf + o, size - o, "]}");
		ipc_broadcast(server, buf);
	}
	free(buf);
}

void switcher_step(struct ts_server *server, uint32_t modifier, int delta) {
	if (server->locked) {
		return;
	}
	if (!server->switcher_active) {
		int count = 0;
		struct ts_view *view;
		wl_list_for_each(view, &server->focus_stack, focus_link) {
			if (view->mapped && count < SWITCHER_MAX) {
				server->switcher_ids[count++] = view->id;
			}
		}
		if (count == 0) {
			return;
		}
		server->switcher_active = true;
		server->switcher_mod = modifier;
		server->switcher_count = count;
		server->switcher_sel = count > 1 ? (delta < 0 ? count - 1 : 1) : 0;
		server->switcher_prev_id = server->focused_view ? server->focused_view->id : 0;
		/* Take the keyboard away from the app while choosing, so it does not
		 * mistake the Alt key going up for a press of Alt on its own. */
		wlr_seat_keyboard_notify_clear_focus(server->seat);
	} else {
		int count = server->switcher_count;
		server->switcher_sel = ((server->switcher_sel + delta) % count + count) % count;
	}
	switcher_send(server);
}

void switcher_finish(struct ts_server *server, bool commit) {
	if (!server->switcher_active) {
		return;
	}
	server->switcher_active = false;
	switcher_send(server);
	uint32_t id = commit ? server->switcher_ids[server->switcher_sel] : server->switcher_prev_id;
	if (view_by_id(server, id)) {
		view_focus_id(server, id);
	} else {
		focus_top_view(server);
	}
	/* The app under the switcher may have had focus already; make sure it
	 * gets the keyboard back. */
	if (server->focused_view && layer_wants_exclusive_focus(server) == NULL) {
		seat_focus_surface(server, server->focused_view->surface);
	}
}

void switcher_modifiers(struct ts_server *server, uint32_t modifiers) {
	if (server->switcher_active && !(modifiers & server->switcher_mod)) {
		switcher_finish(server, true);
	}
}
