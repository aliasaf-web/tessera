#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/util/log.h>
#include "server.h"

static void output_frame(struct wl_listener *listener, void *data) {
	struct ts_output *output = wl_container_of(listener, output, frame);
	if (output->scene_output == NULL) {
		return;
	}
	wlr_scene_output_commit(output->scene_output, NULL);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(output->scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	struct ts_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
	arrange_layers(output);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct ts_output *output = wl_container_of(listener, output, destroy);
	struct ts_server *server = output->server;

	lock_output_destroyed(output);

	struct ts_layer_surface *layer, *tmp;
	for (int i = 0; i < 4; i++) {
		wl_list_for_each_safe(layer, tmp, &output->layers[i], link) {
			layer->output = NULL;
			wl_list_remove(&layer->link);
			wl_list_init(&layer->link);
			wlr_layer_surface_v1_destroy(layer->layer_surface);
		}
	}

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	if (output->background) {
		wlr_scene_node_destroy(&output->background->node);
	}

	/* Move windows to a remaining screen. */
	struct ts_output *fallback = NULL;
	if (!wl_list_empty(&server->outputs)) {
		fallback = wl_container_of(server->outputs.next, fallback, link);
	}
	struct ts_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->output == output) {
			view->output = fallback;
		}
	}
	free(output);
	if (fallback) {
		arrange_all(server);
	}
}

static double auto_scale(struct wlr_output *wlr_output) {
	if (wlr_output->phys_width <= 0 || wlr_output->width <= 0) {
		return 1.0;
	}
	double dpi = wlr_output->width / (wlr_output->phys_width / 25.4);
	if (dpi >= 200) {
		return 2.0;
	} else if (dpi >= 170) {
		return 1.5;
	} else if (dpi >= 150) {
		return 1.25;
	}
	return 1.0;
}

static void new_output(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	if (!wlr_output_init_render(wlr_output, server->allocator, server->renderer)) {
		wlr_log(WLR_ERROR, "Could not set up rendering for %s", wlr_output->name);
		return;
	}

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	} else if (getenv("TESSERA_OUTPUT_SIZE")) {
		/* For testing on virtual screens, e.g. TESSERA_OUTPUT_SIZE=1920x1080 */
		int w = 0, h = 0;
		if (sscanf(getenv("TESSERA_OUTPUT_SIZE"), "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
			wlr_output_state_set_custom_mode(&state, w, h, 0);
		}
	}
	if (!wlr_output_commit_state(wlr_output, &state)) {
		wlr_log(WLR_ERROR, "Could not enable %s", wlr_output->name);
	}
	wlr_output_state_finish(&state);

	/* Scale depends on the size the mode gave us. */
	double scale = server->config.scale > 0 ? server->config.scale : auto_scale(wlr_output);
	if (scale != wlr_output->scale) {
		wlr_output_state_init(&state);
		wlr_output_state_set_scale(&state, scale);
		wlr_output_commit_state(wlr_output, &state);
		wlr_output_state_finish(&state);
	}

	struct ts_output *output = calloc(1, sizeof(*output));
	output->wlr_output = wlr_output;
	output->server = server;
	output->workspace = 1;
	for (int i = 0; i <= TS_WORKSPACES; i++) {
		output->master_ratio[i] = 0.55;
	}
	for (int i = 0; i < 4; i++) {
		wl_list_init(&output->layers[i]);
	}
	wlr_output->data = output;

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	wl_list_insert(server->outputs.prev, &output->link);

	struct wlr_output_layout_output *l_output =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	output->scene_output = wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, l_output, output->scene_output);

	float bg[4] = { server->theme.background.r, server->theme.background.g,
		server->theme.background.b, 1.0f };
	output->background = wlr_scene_rect_create(server->layer_background, 0, 0, bg);

	arrange_layers(output);
	lock_arrange(server);
	wlr_xcursor_manager_load(server->cursor_mgr, scale);
	cursor_rebase(server);
}

struct ts_output *output_at(struct ts_server *server, double lx, double ly) {
	struct wlr_output *wlr_output = wlr_output_layout_output_at(server->output_layout, lx, ly);
	return wlr_output ? wlr_output->data : NULL;
}

struct ts_output *output_focused(struct ts_server *server) {
	if (server->focused_view && server->focused_view->output) {
		return server->focused_view->output;
	}
	struct ts_output *output = output_at(server, server->cursor->x, server->cursor->y);
	if (output) {
		return output;
	}
	if (wl_list_empty(&server->outputs)) {
		return NULL;
	}
	return wl_container_of(server->outputs.next, output, link);
}

void output_set_dpms(struct ts_server *server, bool on) {
	struct ts_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->dpms_off == !on) {
			continue;
		}
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, on);
		if (wlr_output_commit_state(output->wlr_output, &state)) {
			output->dpms_off = !on;
		}
		wlr_output_state_finish(&state);
	}
}

void output_init(struct ts_server *server) {
	wl_list_init(&server->outputs);
	server->new_output.notify = new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);
}
