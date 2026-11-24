#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/util/log.h>
#include "server.h"

static struct wlr_scene_tree *layer_tree(struct ts_server *server, enum zwlr_layer_shell_v1_layer layer) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		return server->layer_shell_bg;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		return server->layer_shell_bottom;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		return server->layer_shell_top;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
	default:
		return server->layer_shell_overlay;
	}
}

static void arrange_layer(struct ts_output *output, struct wl_list *list,
		struct wlr_box *usable, bool exclusive) {
	struct ts_layer_surface *layer;
	wl_list_for_each(layer, list, link) {
		struct wlr_layer_surface_v1 *ls = layer->layer_surface;
		if (!ls->initialized) {
			continue;
		}
		if ((ls->current.exclusive_zone > 0) != exclusive) {
			continue;
		}
		wlr_scene_layer_surface_v1_configure(layer->scene, &output->full_area, usable);
		wlr_scene_node_set_position(&layer->popup_tree->node,
			layer->scene->tree->node.x, layer->scene->tree->node.y);
	}
}

void arrange_layers(struct ts_output *output) {
	struct ts_server *server = output->server;
	struct wlr_box full;
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &full);
	output->full_area = full;
	struct wlr_box usable = full;

	for (int i = 3; i >= 0; i--) {
		arrange_layer(output, &output->layers[i], &usable, true);
	}
	for (int i = 3; i >= 0; i--) {
		arrange_layer(output, &output->layers[i], &usable, false);
	}
	output->usable_area = usable;

	if (output->background) {
		wlr_scene_node_set_position(&output->background->node, full.x, full.y);
		wlr_scene_rect_set_size(output->background, full.width, full.height);
	}
	if (strcmp(server->config.wallpaper, "none") != 0 &&
			(output->wallpaper_w != full.width || output->wallpaper_h != full.height ||
			output->wallpaper_scale != output->wlr_output->scale)) {
		wallpaper_output(output);
	} else if (output->wallpaper) {
		wlr_scene_node_set_position(&output->wallpaper->node, full.x, full.y);
	}

	arrange_output(output);
	lock_arrange(server);
}

struct ts_layer_surface *layer_wants_exclusive_focus(struct ts_server *server) {
	struct ts_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		for (int i = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; i >= ZWLR_LAYER_SHELL_V1_LAYER_TOP; i--) {
			struct ts_layer_surface *layer;
			wl_list_for_each_reverse(layer, &output->layers[i], link) {
				if (layer->mapped && layer->layer_surface->current.keyboard_interactive ==
						ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
					return layer;
				}
			}
		}
	}
	return NULL;
}

static void layer_refocus(struct ts_server *server) {
	struct ts_layer_surface *exclusive = layer_wants_exclusive_focus(server);
	if (exclusive) {
		server->focused_layer = exclusive;
		seat_focus_surface(server, exclusive->layer_surface->surface);
		return;
	}
	server->focused_layer = NULL;
	if (server->focused_view && server->focused_view->mapped) {
		struct ts_view *view = server->focused_view;
		server->focused_view = NULL;
		view_focus(view);
	} else {
		focus_top_view(server);
	}
}

static void layer_map(struct wl_listener *listener, void *data) {
	struct ts_layer_surface *layer = wl_container_of(listener, layer, map);
	layer->mapped = true;
	if (layer->output) {
		arrange_layers(layer->output);
	}
	struct wlr_layer_surface_v1 *ls = layer->layer_surface;
	if (ls->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE &&
			ls->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP && !layer->server->locked) {
		layer->server->focused_layer = layer;
		seat_focus_surface(layer->server, ls->surface);
	}
	cursor_rebase(layer->server);
}

static void layer_unmap(struct wl_listener *listener, void *data) {
	struct ts_layer_surface *layer = wl_container_of(listener, layer, unmap);
	struct ts_server *server = layer->server;
	layer->mapped = false;
	if (layer->output) {
		arrange_layers(layer->output);
	}
	struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
	if (server->focused_layer == layer || focused == layer->layer_surface->surface) {
		server->focused_layer = NULL;
		layer_refocus(server);
	}
	cursor_rebase(server);
}

static void layer_commit(struct wl_listener *listener, void *data) {
	struct ts_layer_surface *layer = wl_container_of(listener, layer, commit);
	struct wlr_layer_surface_v1 *ls = layer->layer_surface;
	if (layer->output == NULL) {
		return;
	}
	uint32_t committed = ls->current.committed;
	if (committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		struct wlr_scene_tree *tree = layer_tree(layer->server, ls->current.layer);
		wlr_scene_node_reparent(&layer->scene->tree->node, tree);
		wl_list_remove(&layer->link);
		wl_list_insert(layer->output->layers[ls->current.layer].prev, &layer->link);
	}
	if (ls->initial_commit || committed || layer->mapped != ls->surface->mapped) {
		arrange_layers(layer->output);
	}
	if ((committed & WLR_LAYER_SURFACE_V1_STATE_KEYBOARD_INTERACTIVITY) && layer->mapped) {
		layer_refocus(layer->server);
	}
}

static void layer_destroy(struct wl_listener *listener, void *data) {
	struct ts_layer_surface *layer = wl_container_of(listener, layer, destroy);
	struct ts_server *server = layer->server;
	wl_list_remove(&layer->link);
	wl_list_remove(&layer->map.link);
	wl_list_remove(&layer->unmap.link);
	wl_list_remove(&layer->commit.link);
	wl_list_remove(&layer->destroy.link);
	wl_list_remove(&layer->new_popup.link);
	wlr_scene_node_destroy(&layer->popup_tree->node);
	if (server->focused_layer == layer) {
		server->focused_layer = NULL;
	}
	struct ts_output *output = layer->output;
	free(layer);
	if (output) {
		arrange_layers(output);
	}
}

static void layer_new_popup(struct wl_listener *listener, void *data) {
	struct ts_layer_surface *layer = wl_container_of(listener, layer, new_popup);
	popup_create(data, layer->popup_tree);
}

static void new_layer_surface(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, new_layer_surface);
	struct wlr_layer_surface_v1 *ls = data;

	if (ls->output == NULL) {
		struct ts_output *output = output_focused(server);
		if (output == NULL) {
			wlr_layer_surface_v1_destroy(ls);
			return;
		}
		ls->output = output->wlr_output;
	}
	struct ts_output *output = ls->output->data;

	struct ts_layer_surface *layer = calloc(1, sizeof(*layer));
	if (layer == NULL) {
		wlr_log(WLR_ERROR, "Out of memory");
		wlr_layer_surface_v1_destroy(ls);
		return;
	}
	layer->node.kind = TS_NODE_LAYER;
	layer->server = server;
	layer->output = output;
	layer->layer_surface = ls;
	layer->scene = wlr_scene_layer_surface_v1_create(
		layer_tree(server, ls->pending.layer), ls);
	layer->scene->tree->node.data = &layer->node;
	layer->popup_tree = wlr_scene_tree_create(server->layer_popups);
	ls->data = layer;

	layer->map.notify = layer_map;
	wl_signal_add(&ls->surface->events.map, &layer->map);
	layer->unmap.notify = layer_unmap;
	wl_signal_add(&ls->surface->events.unmap, &layer->unmap);
	layer->commit.notify = layer_commit;
	wl_signal_add(&ls->surface->events.commit, &layer->commit);
	layer->destroy.notify = layer_destroy;
	wl_signal_add(&ls->events.destroy, &layer->destroy);
	layer->new_popup.notify = layer_new_popup;
	wl_signal_add(&ls->events.new_popup, &layer->new_popup);

	wl_list_insert(output->layers[ls->pending.layer].prev, &layer->link);
}

void layers_init(struct ts_server *server) {
	server->layer_shell = wlr_layer_shell_v1_create(server->display, 4);
	server->new_layer_surface.notify = new_layer_surface;
	wl_signal_add(&server->layer_shell->events.new_surface, &server->new_layer_surface);
}
