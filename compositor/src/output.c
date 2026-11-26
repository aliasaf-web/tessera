#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/types/wlr_output_management_v1.h>
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
	/* Logging rather than checking: this is wlroots asking for a state a tool
	 * requested, and the only answer to "the screen refused it" is to say so -
	 * there is nothing to unwind, and the tool that asked reads the result back
	 * from the output-management test itself. */
	if (!wlr_output_commit_state(output->wlr_output, event->state)) {
		wlr_log(WLR_ERROR, "%s refused the mode it was asked for", output->wlr_output->name);
	}
	if (output->in_layout) {
		arrange_layers(output);
	}
}

/* Panels and the like on a screen that goes away or out of use. */
static void close_layer_surfaces(struct ts_output *output) {
	struct ts_layer_surface *layer, *tmp;
	for (int i = 0; i < 4; i++) {
		wl_list_for_each_safe(layer, tmp, &output->layers[i], link) {
			layer->output = NULL;
			wl_list_remove(&layer->link);
			wl_list_init(&layer->link);
			wlr_layer_surface_v1_destroy(layer->layer_surface);
		}
	}
}

/* A laptop's own screen, going by how it is connected. */
static bool output_is_builtin(struct wlr_output *wlr_output) {
	return strncmp(wlr_output->name, "eDP", 3) == 0 ||
		strncmp(wlr_output->name, "LVDS", 4) == 0 || strncmp(wlr_output->name, "DSI", 3) == 0;
}

/* The laptop's own screen, if it is in use. */
struct ts_output *output_builtin(struct ts_server *server) {
	struct ts_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output_is_builtin(output->wlr_output)) {
			return output;
		}
	}
	return NULL;
}

/* The screen others line up against and mirror: the built-in one if it is
 * in use, otherwise the first screen in use. */
static struct ts_output *main_output(struct ts_server *server) {
	struct ts_output *builtin = output_builtin(server);
	if (builtin != NULL || wl_list_empty(&server->outputs)) {
		return builtin;
	}
	struct ts_output *first = wl_container_of(server->outputs.next, first, link);
	return first;
}

/* A screen in use has its own workspaces, panels and background. */
static void output_join_layout(struct ts_output *output) {
	if (output->in_layout) {
		return;
	}
	struct ts_server *server = output->server;
	wl_list_remove(&output->link);
	wl_list_insert(server->outputs.prev, &output->link);
	output->in_layout = true;
	struct wlr_output_layout_output *l_output =
		wlr_output_layout_add_auto(server->output_layout, output->wlr_output);
	/* A screen in the layout but not in the scene is one that never paints,
	 * which is worse to debug than a desktop that stops and says which screen it
	 * could not set up. `wlr_output_layout_add_auto` gives back NULL only when
	 * the allocator failed, and the scene call below would dereference it, so
	 * this is the last point where anything sensible is left to do. (The scene
	 * call itself has nothing to check: it aborts on the same failure.) */
	if (!l_output) {
		wlr_log(WLR_ERROR, "Out of memory putting %s in the desktop", output->wlr_output->name);
		abort();
	}
	wlr_scene_output_layout_add_output(server->scene_layout, l_output, output->scene_output);
	wlr_scene_node_set_enabled(&output->background->node, true);
}

/* Lets go of everything shown on a screen that goes out of use. */
static void output_forget(struct ts_output *output) {
	struct ts_server *server = output->server;
	lock_output_destroyed(output);
	layout_output_destroyed(output);
	wallpaper_output_destroyed(output);
	close_layer_surfaces(output);
	if (server->ratio_output == output) {
		server->ratio_output = NULL;
		server->cursor_mode = TS_CURSOR_PASSTHROUGH;
	}
	if (server->active_output == output) {
		server->active_output = NULL;
	}
}

/* Out of use (switched off, or mirroring): its windows move to the main
 * screen. */
static void output_leave_layout(struct ts_output *output) {
	if (!output->in_layout) {
		return;
	}
	struct ts_server *server = output->server;
	output_forget(output);
	wlr_scene_node_set_enabled(&output->background->node, false);
	wlr_output_layout_remove(server->output_layout, output->wlr_output);
	wl_list_remove(&output->link);
	wl_list_insert(server->spare_outputs.prev, &output->link);
	output->in_layout = false;
	struct ts_output *fallback = main_output(server);
	struct ts_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->output == output) {
			view->output = fallback;
			if (fallback) {
				view->workspace = fallback->workspace;
			}
		}
	}
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct ts_output *output = wl_container_of(listener, output, destroy);
	struct ts_server *server = output->server;
	output_forget(output);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	if (output->background) {
		wlr_scene_node_destroy(&output->background->node);
	}
	/* Move windows to a remaining screen. */
	struct ts_output *fallback = main_output(server);
	struct ts_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->output == output) {
			view->output = fallback;
		}
	}
	free(output);
	/* A screen that mirrored this one, or was switched off, may now be needed. */
	outputs_apply_config(server);
	arrange_all(server);
}

/* ---------------------------------------------------------------------- */
/* Screen settings: Settings › Displays writes display.PORT.* to the config */
/* file; tools such as wlr-randr use the output management protocol.        */
/* ---------------------------------------------------------------------- */

static const struct ts_display_config *display_config(struct ts_server *server, const char *name) {
	for (int i = 0; i < server->config.display_count; i++) {
		if (strcmp(server->config.displays[i].name, name) == 0) {
			return &server->config.displays[i];
		}
	}
	return NULL;
}

static double auto_scale(struct wlr_output *wlr_output, int width) {
	if (wlr_output->phys_width <= 0 || width <= 0) {
		return 1.0;
	}
	double dpi = width / (wlr_output->phys_width / 25.4);
	if (dpi >= 200) {
		return 2.0;
	} else if (dpi >= 170) {
		return 1.5;
	} else if (dpi >= 150) {
		return 1.25;
	}
	return 1.0;
}

/* The chosen size if the screen offers it (the fastest refresh, unless one
 * was chosen), otherwise the screen's own best mode. */
static struct wlr_output_mode *wanted_mode(struct wlr_output *wlr_output,
		const struct ts_display_config *cfg) {
	struct wlr_output_mode *best = NULL, *mode;
	if (cfg && cfg->width > 0) {
		wl_list_for_each(mode, &wlr_output->modes, link) {
			if (mode->width != cfg->width || mode->height != cfg->height) {
				continue;
			}
			if (best == NULL) {
				best = mode;
			} else if (cfg->refresh > 0) {
				if (abs(mode->refresh - cfg->refresh) < abs(best->refresh - cfg->refresh)) {
					best = mode;
				}
			} else if (mode->refresh > best->refresh) {
				best = mode;
			}
		}
	}
	return best ? best : wlr_output_preferred_mode(wlr_output);
}

/* Switches the screen on with its mode and scale; only what differs is sent,
 * so nothing flickers. scale 0 means: work it out from the settings. */
static void output_switch_on(struct ts_output *output, double scale) {
	struct ts_server *server = output->server;
	struct wlr_output *wlr_output = output->wlr_output;
	if (output->dpms_off) {
		return;
	}
	const struct ts_display_config *cfg = display_config(server, wlr_output->name);
	struct wlr_output_mode *mode = wanted_mode(wlr_output, cfg);
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (!wlr_output->enabled) {
		wlr_output_state_set_enabled(&state, true);
	}
	int width = mode ? mode->width : wlr_output->width;
	if (mode && mode != wlr_output->current_mode) {
		wlr_output_state_set_mode(&state, mode);
	} else if (mode == NULL && getenv("TESSERA_OUTPUT_SIZE")) {
		/* For testing on virtual screens, e.g. TESSERA_OUTPUT_SIZE=1920x1080 */
		int w = 0, h = 0;
		if (sscanf(getenv("TESSERA_OUTPUT_SIZE"), "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
			if (w != wlr_output->width || h != wlr_output->height) {
				wlr_output_state_set_custom_mode(&state, w, h, 0);
			}
			width = w;
		}
	}
	if (scale <= 0) {
		scale = cfg && cfg->scale > 0 ? cfg->scale
			: server->config.scale > 0 ? server->config.scale : auto_scale(wlr_output, width);
	}
	if (fabs(scale - wlr_output->scale) > 1e-4) {
		wlr_output_state_set_scale(&state, scale);
	}
	if (state.committed && !wlr_output_commit_state(wlr_output, &state)) {
		struct wlr_output_mode *preferred = wlr_output_preferred_mode(wlr_output);
		wlr_log(WLR_ERROR, "%s can't use the chosen mode; using its own", wlr_output->name);
		if (preferred && preferred != mode) {
			wlr_output_state_set_mode(&state, preferred);
			/* The screen's own preferred mode is the last thing left to try; if
			 * that is refused too there is no setting to fall back to, so say it
			 * and let the output carry on in whatever state it is in. */
			if (!wlr_output_commit_state(wlr_output, &state)) {
				wlr_log(WLR_ERROR, "%s refused even its preferred mode", wlr_output->name);
			}
		}
	}
	wlr_output_state_finish(&state);
	wlr_xcursor_manager_load(server->cursor_mgr, wlr_output->scale);
}

static void output_switch_off(struct ts_output *output) {
	if (!output->wlr_output->enabled) {
		return;
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, false);
	/* The caller has already decided the screen is dark - DPMS and the sleep
	 * path both count on it - so a refusal is worth a line: it is the only sign
	 * that a laptop lid closed over a screen still lit and still drawing. */
	if (!wlr_output_commit_state(output->wlr_output, &state)) {
		wlr_log(WLR_ERROR, "%s would not turn off", output->wlr_output->name);
	}
	wlr_output_state_finish(&state);
}

/* Lines the screens in use up next to the main one, as chosen, then moves
 * them all so nothing is left or above 0,0. */
static void place_outputs(struct ts_server *server) {
	struct ts_output *main = main_output(server);
	if (main == NULL) {
		return;
	}
	struct placed {
		struct ts_output *output;
		struct wlr_box box;
	} placed[TS_MAX_DISPLAYS * 2];
	int count = 0;
	struct wlr_box extent = {0};
	struct ts_output *output;
	/* The main screen first, so the others can line up against it. */
	for (int pass = 0; pass < 2; pass++) {
		wl_list_for_each(output, &server->outputs, link) {
			if ((pass == 0) != (output == main) || count == (int)(sizeof(placed) / sizeof(placed[0]))) {
				continue;
			}
			const struct ts_display_config *cfg = display_config(server, output->wlr_output->name);
			struct wlr_box box = {0};
			wlr_output_effective_resolution(output->wlr_output, &box.width, &box.height);
			enum ts_display_place place = cfg ? cfg->place : TS_PLACE_AUTO;
			if (place == TS_PLACE_AT) {
				box.x = cfg->x;
				box.y = cfg->y;
			} else if (output == main) {
				box.x = box.y = 0;
			} else if (place == TS_PLACE_LEFT) {
				box.x = extent.x - box.width;
				box.y = placed[0].box.y;
			} else if (place == TS_PLACE_ABOVE) {
				box.x = placed[0].box.x;
				box.y = extent.y - box.height;
			} else if (place == TS_PLACE_BELOW) {
				box.x = placed[0].box.x;
				box.y = extent.y + extent.height;
			} else {
				box.x = extent.x + extent.width;
				box.y = placed[0].box.y;
			}
			if (count == 0) {
				extent = box;
			} else {
				int x1 = MIN(extent.x, box.x), y1 = MIN(extent.y, box.y);
				int x2 = MAX(extent.x + extent.width, box.x + box.width);
				int y2 = MAX(extent.y + extent.height, box.y + box.height);
				extent = (struct wlr_box){ x1, y1, x2 - x1, y2 - y1 };
			}
			placed[count++] = (struct placed){ output, box };
		}
	}
	for (int i = 0; i < count; i++) {
		struct wlr_box box = placed[i].box;
		wlr_output_layout_add(server->output_layout, placed[i].output->wlr_output,
			box.x - extent.x, box.y - extent.y);
	}
}

/* A mirroring screen shows the main screen's area, scaled so all of it fits
 * (with dark bands where the shapes differ). It has no pointer of its own. */
static void update_mirrors(struct ts_server *server) {
	struct ts_output *main = main_output(server);
	if (main == NULL) {
		return;
	}
	struct wlr_box area;
	wlr_output_layout_get_box(server->output_layout, main->wlr_output, &area);
	if (wlr_box_empty(&area)) {
		return;
	}
	struct ts_output *output;
	wl_list_for_each(output, &server->spare_outputs, link) {
		struct wlr_output *wlr_output = output->wlr_output;
		if (!output->mirror || output->dpms_off) {
			continue;
		}
		struct wlr_output_mode *mode =
			wanted_mode(wlr_output, display_config(server, wlr_output->name));
		int mw = mode ? mode->width : wlr_output->width;
		int mh = mode ? mode->height : wlr_output->height;
		if (mw <= 0 || mh <= 0) {
			output_switch_on(output, 0); /* a virtual screen gets its size here */
			mw = wlr_output->width;
			mh = wlr_output->height;
			if (mw <= 0 || mh <= 0) {
				continue;
			}
		}
		output_switch_on(output, fmin((double)mw / area.width, (double)mh / area.height));
		int w, h;
		wlr_output_effective_resolution(wlr_output, &w, &h);
		wlr_scene_output_set_position(output->scene_output,
			area.x - (w - area.width) / 2, area.y - (h - area.height) / 2);
	}
}

/* Tells tools like wlr-randr how the screens are set up now. */
static void update_output_manager(struct ts_server *server) {
	if (server->output_manager == NULL) {
		return;
	}
	struct wlr_output_configuration_v1 *config = wlr_output_configuration_v1_create();
	if (config == NULL) {
		return;
	}
	struct wl_list *lists[] = { &server->outputs, &server->spare_outputs };
	for (int i = 0; i < 2; i++) {
		struct ts_output *output;
		wl_list_for_each(output, lists[i], link) {
			struct wlr_output_configuration_head_v1 *head =
				wlr_output_configuration_head_v1_create(config, output->wlr_output);
			if (head == NULL) {
				continue;
			}
			struct wlr_box box;
			wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
			head->state.enabled = !output->off;
			head->state.x = box.x;
			head->state.y = box.y;
		}
	}
	wlr_output_manager_v1_set_configuration(server->output_manager, config);
}

/* Applies the screen settings to every screen. Called at start-up, when a
 * screen is plugged in or out, and when the settings change. */
void outputs_apply_config(struct ts_server *server) {
	struct ts_output *all[TS_MAX_DISPLAYS * 2];
	int count = 0;
	struct wl_list *lists[] = { &server->outputs, &server->spare_outputs };
	for (int i = 0; i < 2; i++) {
		struct ts_output *output;
		wl_list_for_each(output, lists[i], link) {
			if (count < (int)(sizeof(all) / sizeof(all[0]))) {
				all[count++] = output;
			}
		}
	}
	/* What each screen should do. One screen always stays in use: the
	 * built-in one if possible. */
	int in_use = 0, keep = 0;
	for (int i = 0; i < count; i++) {
		const struct ts_display_config *cfg = display_config(server, all[i]->wlr_output->name);
		all[i]->off = cfg && cfg->off;
		all[i]->mirror = !all[i]->off && cfg && cfg->place == TS_PLACE_MIRROR;
		if (!all[i]->off && !all[i]->mirror) {
			in_use++;
		}
		if (output_is_builtin(all[i]->wlr_output) && !output_is_builtin(all[keep]->wlr_output)) {
			keep = i;
		}
	}
	if (in_use == 0 && count > 0) {
		all[keep]->off = all[keep]->mirror = false;
	}
	for (int i = 0; i < count; i++) {
		struct ts_output *output = all[i];
		if (output->off || output->mirror) {
			output_leave_layout(output);
		}
		if (output->off) {
			output_switch_off(output);
		} else if (!output->mirror) {
			output_switch_on(output, 0);
			output_join_layout(output);
		}
	}
	place_outputs(server);
	update_mirrors(server);
	update_output_manager(server);
	input_map_devices(server);
	ipc_broadcast(server, "{\"event\":\"outputs\"}");
}

static void layout_change(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, layout_change);
	update_mirrors(server);
	update_output_manager(server);
}

/* A tool such as wlr-randr asks for a new set-up. It lasts until the
 * settings are applied again (when they change, or at the next login). */
static bool apply_from_tool(struct ts_server *server, struct wlr_output_configuration_v1 *config,
		bool test_only) {
	struct wlr_output_configuration_head_v1 *head;
	int enabled = 0;
	wl_list_for_each(head, &config->heads, link) {
		enabled += head->state.enabled;
	}
	if (enabled == 0) {
		return false; /* never switch every screen off */
	}
	bool ok = true;
	wl_list_for_each(head, &config->heads, link) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_head_v1_state_apply(&head->state, &state);
		ok = ok && wlr_output_test_state(head->state.output, &state);
		wlr_output_state_finish(&state);
	}
	if (!ok || test_only) {
		return ok;
	}
	wl_list_for_each(head, &config->heads, link) {
		struct ts_output *output = head->state.output->data;
		if (!head->state.enabled) {
			output_leave_layout(output);
			output->off = true;
			output->mirror = false;
		}
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_head_v1_state_apply(&head->state, &state);
		ok = wlr_output_commit_state(head->state.output, &state) && ok;
		wlr_output_state_finish(&state);
		if (head->state.enabled) {
			output->off = output->mirror = false;
			bool was_in_use = output->in_layout;
			output_join_layout(output); /* a new screen goes where it fits */
			/* A tool that only changes a screen's mode or size has no position
			 * to give, and the protocol then says 0,0 - moving every screen in
			 * the request to that corner would pile the screens on top of each
			 * other. So an all-zero position is only followed when it is where
			 * the screen already is; a position meant on purpose goes in the
			 * settings file (display.X.position=0,0), which is read in full. */
			struct wlr_box box = {0};
			if (was_in_use) {
				wlr_output_layout_get_box(server->output_layout, head->state.output, &box);
			}
			if (was_in_use && (box.x != head->state.x || box.y != head->state.y) &&
					(head->state.x != 0 || head->state.y != 0)) {
				wlr_output_layout_add(server->output_layout, head->state.output,
					head->state.x, head->state.y);
			}
		}
	}
	arrange_all(server);
	wallpaper_update(server);
	update_output_manager(server);
	input_map_devices(server);
	ipc_broadcast(server, "{\"event\":\"outputs\"}");
	return ok;
}

/* A program's request to move, resize or switch off a screen. While the screen
 * is locked these are refused: nothing you started hours ago should get to
 * change what the lock screen is drawn on, and a layout that moves under a
 * locked screen hides it from you rather than protecting you. */
static bool can_change_outputs(struct ts_server *server) {
	if (server->locked) {
		wlr_log(WLR_DEBUG, "Refused to change the screens while the screen is locked");
		return false;
	}
	return true;
}

static void output_manager_apply(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, output_manager_apply);
	struct wlr_output_configuration_v1 *config = data;
	if (!can_change_outputs(server)) {
		wlr_output_configuration_v1_send_failed(config);
		wlr_output_configuration_v1_destroy(config);
		return;
	}
	if (apply_from_tool(server, config, false)) {
		wlr_output_configuration_v1_send_succeeded(config);
	} else {
		wlr_output_configuration_v1_send_failed(config);
	}
	wlr_output_configuration_v1_destroy(config);
}

static void output_manager_test(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, output_manager_test);
	struct wlr_output_configuration_v1 *config = data;
	if (!can_change_outputs(server)) {
		wlr_output_configuration_v1_send_failed(config);
		wlr_output_configuration_v1_destroy(config);
		return;
	}
	if (apply_from_tool(server, config, true)) {
		wlr_output_configuration_v1_send_succeeded(config);
	} else {
		wlr_output_configuration_v1_send_failed(config);
	}
	wlr_output_configuration_v1_destroy(config);
}

/* The screens, for Settings › Displays: {"outputs":[...]} on one line. */
char *outputs_json(struct ts_server *server) {
	char *text = NULL;
	size_t size = 0;
	FILE *f = open_memstream(&text, &size);
	if (f == NULL) {
		return NULL;
	}
	struct ts_output *main = main_output(server);
	fputs("{\"outputs\":[", f);
	bool first = true;
	struct wl_list *lists[] = { &server->outputs, &server->spare_outputs };
	for (int i = 0; i < 2; i++) {
		struct ts_output *output;
		wl_list_for_each(output, lists[i], link) {
			struct wlr_output *o = output->wlr_output;
			char name[96], make[192], model[192];
			json_escape(name, sizeof(name), o->name);
			json_escape(make, sizeof(make), o->make ? o->make : "");
			json_escape(model, sizeof(model), o->model ? o->model : "");
			struct wlr_box box = {0};
			if (output->in_layout) {
				wlr_output_layout_get_box(server->output_layout, o, &box);
			}
			fprintf(f, "%s{\"name\":\"%s\",\"make\":\"%s\",\"model\":\"%s\",\"builtin\":%s,"
				"\"main\":%s,\"enabled\":%s,\"mirror\":%s,\"width\":%d,\"height\":%d,"
				"\"refresh\":%d,\"scale\":%.4f,\"x\":%d,\"y\":%d,\"logical_width\":%d,"
				"\"logical_height\":%d,\"phys_width\":%d,\"phys_height\":%d,\"modes\":[",
				first ? "" : ",", name, make, model,
				output_is_builtin(o) ? "true" : "false", output == main ? "true" : "false",
				output->off ? "false" : "true", output->mirror ? "true" : "false",
				o->width, o->height, o->refresh, o->scale, box.x, box.y, box.width,
				box.height, o->phys_width, o->phys_height);
			first = false;
			bool first_mode = true;
			struct wlr_output_mode *mode;
			wl_list_for_each(mode, &o->modes, link) {
				fprintf(f, "%s{\"width\":%d,\"height\":%d,\"refresh\":%d,\"preferred\":%s}",
					first_mode ? "" : ",", mode->width, mode->height, mode->refresh,
					mode->preferred ? "true" : "false");
				first_mode = false;
			}
			fputs("]}", f);
		}
	}
	fputs("]}", f);
	fclose(f);
	return text;
}

static void new_output(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	if (!wlr_output_init_render(wlr_output, server->allocator, server->renderer)) {
		wlr_log(WLR_ERROR, "Could not set up rendering for %s", wlr_output->name);
		return;
	}
	struct ts_output *output = calloc(1, sizeof(*output));
	if (output == NULL) {
		wlr_log(WLR_ERROR, "Out of memory");
		return;
	}
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

	/* Out of use until the settings put it to use. */
	wl_list_insert(server->spare_outputs.prev, &output->link);
	output->scene_output = wlr_scene_output_create(server->scene, wlr_output);
	float bg[4] = { server->theme.background.r, server->theme.background.g,
		server->theme.background.b, 1.0f };
	output->background = wlr_scene_rect_create(server->layer_background, 0, 0, bg);
	wlr_scene_node_set_enabled(&output->background->node, false);

	outputs_apply_config(server);
	arrange_all(server);
	lock_arrange(server);
	night_light_apply_output(output);
	cursor_rebase(server);
}

struct ts_output *output_at(struct ts_server *server, double lx, double ly) {
	struct wlr_output *wlr_output = wlr_output_layout_output_at(server->output_layout, lx, ly);
	return wlr_output ? wlr_output->data : NULL;
}

/* A screen in use, by its connector name (such as HDMI-A-1). */
struct ts_output *output_by_name(struct ts_server *server, const char *name) {
	struct ts_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (strcmp(output->wlr_output->name, name) == 0) {
			return output;
		}
	}
	return NULL;
}

/* The screen in use: the focused window's, else the one picked last, else
 * the one under the pointer. */
struct ts_output *output_focused(struct ts_server *server) {
	if (server->focused_view && server->focused_view->output) {
		return server->focused_view->output;
	}
	if (server->active_output) {
		return server->active_output;
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

/* Screens off when idle; mirrors follow, screens switched off stay off. */
void output_set_dpms(struct ts_server *server, bool on) {
	struct wl_list *lists[] = { &server->outputs, &server->spare_outputs };
	for (int i = 0; i < 2; i++) {
		struct ts_output *output;
		wl_list_for_each(output, lists[i], link) {
			if (output->dpms_off == !on || output->off) {
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
	if (on) {
		arrange_all(server);
		update_mirrors(server);
		night_light_update(server, true);
	}
}

void output_init(struct ts_server *server) {
	wl_list_init(&server->outputs);
	wl_list_init(&server->spare_outputs);
	server->new_output.notify = new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);
	server->layout_change.notify = layout_change;
	wl_signal_add(&server->output_layout->events.change, &server->layout_change);
	server->output_manager = wlr_output_manager_v1_create(server->display);
	server->output_manager_apply.notify = output_manager_apply;
	wl_signal_add(&server->output_manager->events.apply, &server->output_manager_apply);
	server->output_manager_test.notify = output_manager_test;
	wl_signal_add(&server->output_manager->events.test, &server->output_manager_test);
}
