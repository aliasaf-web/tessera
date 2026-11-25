#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include "server.h"

/* ---------------------------------------------------------------------- */
/* Type dispatch                                                            */
/* ---------------------------------------------------------------------- */

const char *view_title(struct ts_view *view) {
	const char *title = NULL;
	if (view->type == TS_VIEW_XDG) {
		title = view->xdg_toplevel->title;
	}
#if TESSERA_XWAYLAND
	else if (view->xsurface) {
		title = view->xsurface->title;
	}
#endif
	return title ? title : "";
}

const char *view_app_id(struct ts_view *view) {
	const char *id = NULL;
	if (view->type == TS_VIEW_XDG) {
		id = view->xdg_toplevel->app_id;
	}
#if TESSERA_XWAYLAND
	else if (view->xsurface) {
		id = view->xsurface->class;
	}
#endif
	return id ? id : "";
}

void view_get_geometry(struct ts_view *view, struct wlr_box *geo) {
	*geo = (struct wlr_box){0};
	if (view->type == TS_VIEW_XDG) {
		wlr_xdg_surface_get_geometry(view->xdg_toplevel->base, geo);
	}
#if TESSERA_XWAYLAND
	else if (view->xsurface && view->xsurface->surface) {
		geo->width = view->xsurface->surface->current.width;
		geo->height = view->xsurface->surface->current.height;
	}
#endif
}

static void view_set_activated(struct ts_view *view, bool activated) {
	if (view->type == TS_VIEW_XDG) {
		wlr_xdg_toplevel_set_activated(view->xdg_toplevel, activated);
	}
#if TESSERA_XWAYLAND
	else if (view->xsurface) {
		wlr_xwayland_surface_activate(view->xsurface, activated);
		if (activated) {
			wlr_xwayland_surface_restack(view->xsurface, NULL, XCB_STACK_MODE_ABOVE);
		}
	}
#endif
}

void view_send_size(struct ts_view *view, int width, int height) {
	if (width < 1 || height < 1) {
		return;
	}
	if (view->type == TS_VIEW_XDG) {
		if (view->requested_w != width || view->requested_h != height) {
			wlr_xdg_toplevel_set_size(view->xdg_toplevel, width, height);
		}
	}
#if TESSERA_XWAYLAND
	else if (view->xsurface) {
		int lx = view->box.x + view->content.x;
		int ly = view->box.y + view->content.y;
		wlr_xwayland_surface_configure(view->xsurface, lx, ly, width, height);
	}
#endif
	view->requested_w = width;
	view->requested_h = height;
}

static void view_send_tiled(struct ts_view *view, bool tiled) {
	if (view->type == TS_VIEW_XDG) {
		wlr_xdg_toplevel_set_tiled(view->xdg_toplevel,
			tiled ? (WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT)
				: WLR_EDGE_NONE);
	}
}

void view_close(struct ts_view *view) {
	if (view == NULL) {
		return;
	}
	if (view->type == TS_VIEW_XDG) {
		wlr_xdg_toplevel_send_close(view->xdg_toplevel);
	}
#if TESSERA_XWAYLAND
	else if (view->xsurface) {
		wlr_xwayland_surface_close(view->xsurface);
	}
#endif
}

struct ts_view *view_from_surface(struct ts_server *server, struct wlr_surface *surface) {
	if (surface == NULL) {
		return NULL;
	}
	struct wlr_surface *root = wlr_surface_get_root_surface(surface);
	struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(root);
	while (xdg && xdg->role == WLR_XDG_SURFACE_ROLE_POPUP && xdg->popup && xdg->popup->parent) {
		struct wlr_surface *parent = wlr_surface_get_root_surface(xdg->popup->parent);
		xdg = wlr_xdg_surface_try_from_wlr_surface(parent);
	}
	if (xdg && xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && xdg->data) {
		struct wlr_scene_tree *tree = xdg->data;
		struct ts_node *node = tree->node.data;
		if (node && node->kind == TS_NODE_VIEW) {
			return (struct ts_view *)node;
		}
	}
#if TESSERA_XWAYLAND
	struct wlr_xwayland_surface *xs = wlr_xwayland_surface_try_from_wlr_surface(root);
	if (xs && xs->data) {
		return xs->data;
	}
#endif
	(void)server;
	return NULL;
}

/* ---------------------------------------------------------------------- */
/* Frame and placement                                                      */
/* ---------------------------------------------------------------------- */

static struct wlr_scene_tree *layer_for_view(struct ts_view *view) {
	struct ts_server *server = view->server;
	if (view->fullscreen) {
		return server->layer_fullscreen;
	}
	if (view->floating || view->maximized) {
		return server->layer_floating;
	}
	return server->layer_tiled;
}

static int inset_side(struct ts_view *view) {
	return view->fullscreen ? 0 : deco_border();
}

static int inset_top(struct ts_view *view) {
	if (view->fullscreen) {
		return 0;
	}
	return view->ssd ? deco_titlebar_height() : deco_border();
}

void view_update_frame(struct ts_view *view) {
	struct ts_server *server = view->server;
	struct wlr_box b = view->box;
	int B = inset_side(view);
	int T = inset_top(view);
	struct wlr_box slot = { B, T, MAX(1, b.width - 2 * B), MAX(1, b.height - T - B) };

	struct wlr_box geo;
	view_get_geometry(view, &geo);
	int cw = geo.width > 0 ? MIN(geo.width, slot.width) : slot.width;
	int ch = geo.height > 0 ? MIN(geo.height, slot.height) : slot.height;
	int ox = slot.x + (slot.width - cw) / 2;
	int oy = slot.y + (slot.height - ch) / 2;
	if (!view->fullscreen && geo.height > 0 && geo.height < slot.height && !view->floating) {
		/* Keep short windows attached to their title bar. */
		oy = slot.y;
	}
	view->content = (struct wlr_box){ ox, oy, cw, ch };

	/* The xdg scene tree already shifts the surface so that its origin is
	 * the window geometry; clipping happens in surface coordinates. */
	wlr_scene_node_set_position(&view->tree->node, b.x, b.y);
	wlr_scene_node_set_position(&view->content_tree->node, ox, oy);
	if (view->surface && !wl_list_empty(&view->content_tree->children)) {
		struct wlr_box clip = { geo.x, geo.y, cw, ch };
		wlr_scene_subsurface_tree_set_clip(&view->content_tree->node, &clip);
	}
	wlr_scene_node_set_position(&view->popup_tree->node, b.x + ox, b.y + oy);

#if TESSERA_XWAYLAND
	if (view->type == TS_VIEW_XWAYLAND && view->xsurface && view->mapped) {
		int lx = b.x + ox, ly = b.y + oy;
		if (view->xsurface->x != lx || view->xsurface->y != ly) {
			wlr_xwayland_surface_configure(view->xsurface, lx, ly,
				view->xsurface->width, view->xsurface->height);
		}
	}
#endif

	double scale = view->output ? view->output->wlr_output->scale : 1.0;
	bool focused = server->focused_view == view;

	if (view->fullscreen) {
		wlr_scene_node_set_enabled(&view->frame->node, false);
		wlr_scene_node_set_enabled(&view->fs_bg->node, true);
		wlr_scene_rect_set_size(view->fs_bg, b.width, b.height);
		for (int i = 0; i < 4; i++) {
			wlr_scene_node_set_enabled(&view->corners[i]->node, false);
		}
		return;
	}
	wlr_scene_node_set_enabled(&view->fs_bg->node, false);
	wlr_scene_node_set_enabled(&view->frame->node, true);

	/* Background and outline are rectangles: only their sizes change when
	 * the window is resized. */
	struct ts_theme *t = &server->theme;
	float surface[4] = { t->surface.r, t->surface.g, t->surface.b, t->surface.a };
	wlr_scene_rect_set_color(view->frame_bg, surface);
	int bg_y = view->ssd ? T : 0;
	wlr_scene_rect_set_size(view->frame_bg, b.width, b.height - bg_y);
	wlr_scene_node_set_position(&view->frame_bg->node, 0, bg_y);

	int e;
	struct ts_color oc = deco_outline_color(server, focused, view->floating, &e);
	float outline[4] = { oc.r, oc.g, oc.b, oc.a };
	/* Sides start below the title bar (it draws its own outline) or below
	 * the top edge, so no pixel is covered twice. */
	int side_y = view->ssd ? T : e;
	struct wlr_box edges[4] = {
		{ 0, 0, b.width, e },                              /* top */
		{ b.width - e, side_y, e, b.height - e - side_y }, /* right */
		{ 0, b.height - e, b.width, e },                   /* bottom */
		{ 0, side_y, e, b.height - e - side_y },           /* left */
	};
	for (int i = 0; i < 4; i++) {
		bool shown = e > 0 && edges[i].height > 0 && !(i == 0 && view->ssd);
		wlr_scene_node_set_enabled(&view->edges[i]->node, shown);
		if (shown) {
			wlr_scene_rect_set_color(view->edges[i], outline);
			wlr_scene_rect_set_size(view->edges[i], edges[i].width, edges[i].height);
			wlr_scene_node_set_position(&view->edges[i]->node, edges[i].x, edges[i].y);
		}
	}

	/* The title bar is the only drawn image; it changes with the width, focus,
	 * title, close button hover and theme, not with the height. */
	bool restyle = view->frame_focused != focused || view->frame_scale != scale;
	wlr_scene_node_set_enabled(&view->titlebar->node, view->ssd);
	if (view->ssd && (view->frame_dirty || restyle || view->frame_w != b.width ||
			view->titlebar->buffer == NULL)) {
		struct wlr_buffer *buf = deco_render_titlebar(server, b.width, scale,
			view_title(view), focused, view->frame_hover_close, view->floating);
		if (buf) {
			wlr_scene_buffer_set_buffer(view->titlebar, buf);
			wlr_scene_buffer_set_dest_size(view->titlebar, b.width, T);
			wlr_buffer_drop(buf);
		}
		view->frame_w = b.width;
	}
	view->frame_dirty = false;

	int r = deco_radius();
	int pos[4][2] = { {0, 0}, {b.width - r, 0}, {b.width - r, b.height - r}, {0, b.height - r} };

	/* Without a wallpaper the corners are shared images. With one they show
	 * the part of it behind them, so they follow the window; while a divider
	 * is being dragged they are hidden and drawn once it is let go. */
	struct ts_output *output = view->output;
	bool has_wallpaper = output && output->wallpaper_surface;
	bool stale = view->corners_dirty || restyle || view->corners[0]->buffer == NULL ||
		(has_wallpaper && !wlr_box_equal(&view->corner_box, &b));
	bool hide_corners = false;
	if (stale && has_wallpaper && server->cursor_mode == TS_CURSOR_RATIO) {
		hide_corners = true;
	} else if (stale) {
		for (int i = 0; i < 4; i++) {
			if (has_wallpaper) {
				double ox = b.x + pos[i][0] - output->full_area.x;
				double oy = b.y + pos[i][1] - output->full_area.y;
				struct wlr_buffer *cbuf = deco_render_corner(server, i, scale, focused,
					output, ox, oy);
				if (cbuf) {
					wlr_scene_buffer_set_buffer(view->corners[i], cbuf);
					wlr_buffer_drop(cbuf);
				}
			} else {
				wlr_scene_buffer_set_buffer(view->corners[i],
					deco_shared_corner(server, i, scale, focused));
			}
			wlr_scene_buffer_set_dest_size(view->corners[i], r, r);
		}
		view->corner_box = b;
		view->corners_dirty = false;
	}
	view->frame_focused = focused;
	view->frame_scale = scale;
	for (int i = 0; i < 4; i++) {
		bool top = i < 2;
		bool enabled = !hide_corners && !view->floating && !(top && view->ssd);
		wlr_scene_node_set_enabled(&view->corners[i]->node, enabled);
		wlr_scene_node_set_position(&view->corners[i]->node, pos[i][0], pos[i][1]);
	}
}

void views_redraw_frames(struct ts_server *server) {
	struct ts_view *view;
	wl_list_for_each(view, &server->views, link) {
		view->frame_dirty = true;
		view->corners_dirty = true;
		view_update_frame(view);
	}
}

void view_set_box(struct ts_view *view, struct wlr_box box) {
	view->box = box;
	int B = inset_side(view);
	int T = inset_top(view);
	view_send_size(view, box.width - 2 * B, box.height - T - B);
	view_update_frame(view);
}

/* Floating windows are sized by the app: wrap the frame around it. */
void view_fit_floating(struct ts_view *view) {
	if (!view->floating || view->fullscreen || view->maximized) {
		return;
	}
	struct wlr_box geo;
	view_get_geometry(view, &geo);
	if (geo.width <= 0 || geo.height <= 0) {
		return;
	}
	int w = geo.width + 2 * inset_side(view);
	int h = geo.height + inset_top(view) + inset_side(view);
	if (w != view->box.width || h != view->box.height) {
		view->box.width = w;
		view->box.height = h;
		view->float_box = view->box;
	}
	view_update_frame(view);
}

void view_raise(struct ts_view *view) {
	wlr_scene_node_raise_to_top(&view->tree->node);
	wlr_scene_node_raise_to_top(&view->popup_tree->node);
}

/* ---------------------------------------------------------------------- */
/* Focus                                                                    */
/* ---------------------------------------------------------------------- */

void view_focus(struct ts_view *view) {
	if (view == NULL || !view->mapped) {
		return;
	}
	struct ts_server *server = view->server;
	if (server->locked) {
		return;
	}
	struct ts_view *prev = server->focused_view;
	server->focused_view = view;
	if (prev && prev != view && prev->mapped) {
		view_set_activated(prev, false);
		view_update_frame(prev);
	}
	wl_list_remove(&view->focus_link);
	wl_list_insert(&server->focus_stack, &view->focus_link);
	view_set_activated(view, true);
	view_raise(view);
	view->urgent = false;
	if (layer_wants_exclusive_focus(server) == NULL) {
		server->focused_layer = NULL;
		seat_focus_surface(server, view->surface);
	}
	view_update_frame(view);
	ipc_send_state(server);
}

bool view_is_hidden(struct ts_view *view) {
	return view->output != NULL && view->workspace != view->output->workspace;
}

struct ts_view *view_by_id(struct ts_server *server, uint32_t id) {
	struct ts_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->id == id && view->mapped) {
			return view;
		}
	}
	return NULL;
}

/* Focus a window, going to its workspace first if needed. */
void view_focus_id(struct ts_server *server, uint32_t id) {
	struct ts_view *view = view_by_id(server, id);
	if (view == NULL) {
		return;
	}
	if (view->output && view->workspace != view->output->workspace) {
		workspace_switch_output(view->output, view->workspace);
	}
	view_focus(view);
}

void focus_top_view(struct ts_server *server) {
	struct ts_view *view;
	wl_list_for_each(view, &server->focus_stack, focus_link) {
		if (view->mapped && view_visible(view)) {
			view_focus(view);
			return;
		}
	}
	server->focused_view = NULL;
	if (layer_wants_exclusive_focus(server) == NULL && !server->locked) {
		seat_focus_surface(server, NULL);
	}
	ipc_send_state(server);
}

/* ---------------------------------------------------------------------- */
/* State changes                                                            */
/* ---------------------------------------------------------------------- */

static void view_reparent(struct ts_view *view) {
	wlr_scene_node_reparent(&view->tree->node, layer_for_view(view));
}

static void center_float_box(struct ts_view *view) {
	struct ts_output *output = view->output;
	if (output == NULL) {
		return;
	}
	struct wlr_box area = output->usable_area;
	struct wlr_box *fb = &view->float_box;
	if (fb->width <= 0 || fb->height <= 0) {
		fb->width = area.width * 6 / 10;
		fb->height = area.height * 7 / 10;
	}
	fb->width = MIN(fb->width, area.width);
	fb->height = MIN(fb->height, area.height);
	fb->x = area.x + (area.width - fb->width) / 2;
	fb->y = area.y + (area.height - fb->height) / 2;
}

/* A floating window that leaves full screen or maximized gets back the size
 * and place it had before (the app would otherwise keep the big size). */
static void restore_floating_size(struct ts_view *view) {
	if (view->floating && !view->fullscreen && !view->maximized &&
			view->float_box.width > 0 && view->float_box.height > 0) {
		view_set_box(view, view->float_box);
	}
}

void view_set_fullscreen(struct ts_view *view, bool fullscreen) {
	if (view->fullscreen == fullscreen) {
		return;
	}
	if (fullscreen) {
		/* Only one fullscreen window per workspace. */
		struct ts_view *other;
		wl_list_for_each(other, &view->server->views, link) {
			if (other != view && other->fullscreen && other->output == view->output &&
					other->workspace == view->workspace) {
				view_set_fullscreen(other, false);
			}
		}
	}
	view->fullscreen = fullscreen;
	view->frame_dirty = true;
	if (view->type == TS_VIEW_XDG) {
		wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, fullscreen);
	}
#if TESSERA_XWAYLAND
	else if (view->xsurface) {
		wlr_xwayland_surface_set_fullscreen(view->xsurface, fullscreen);
	}
#endif
	restore_floating_size(view);
	view_reparent(view);
	if (view->output) {
		arrange_output(view->output);
	}
	cursor_rebase(view->server);
}

void view_set_maximized(struct ts_view *view, bool maximized) {
	if (view->maximized == maximized) {
		return;
	}
	view->maximized = maximized;
	view->frame_dirty = true;
	if (view->type == TS_VIEW_XDG) {
		wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, maximized);
	}
#if TESSERA_XWAYLAND
	else if (view->xsurface) {
		wlr_xwayland_surface_set_maximized(view->xsurface, maximized);
	}
#endif
	restore_floating_size(view);
	view_reparent(view);
	if (view->output) {
		arrange_output(view->output);
	}
	view_raise(view);
	cursor_rebase(view->server);
}

void view_set_floating(struct ts_view *view, bool floating) {
	if (view->floating == floating) {
		return;
	}
	if (floating && view->float_box.width <= 0) {
		center_float_box(view);
	}
	view->floating = floating;
	view->frame_dirty = true;
	view_send_tiled(view, !floating);
	view_reparent(view);
	if (floating) {
		view->box = view->float_box;
		/* Ask for the remembered size; the frame follows what the app picks. */
		view_set_box(view, view->float_box);
	}
	if (view->output) {
		arrange_output(view->output);
	}
	view_raise(view);
	cursor_rebase(view->server);
}

void view_move_to_workspace(struct ts_view *view, int workspace) {
	if (workspace < 1 || workspace > TS_WORKSPACES || view->workspace == workspace) {
		return;
	}
	view->workspace = workspace;
	if (view->output) {
		arrange_output(view->output);
	}
	focus_top_view(view->server);
	ipc_send_state(view->server);
}

/* ---------------------------------------------------------------------- */
/* Shared lifecycle                                                         */
/* ---------------------------------------------------------------------- */

struct ts_view *view_create_common(struct ts_server *server, enum ts_view_type type) {
	struct ts_view *view = calloc(1, sizeof(*view));
	if (view == NULL) {
		wlr_log(WLR_ERROR, "Out of memory");
		return NULL;
	}
	view->node.kind = TS_NODE_VIEW;
	view->id = ++server->next_view_id;
	view->server = server;
	view->type = type;
	view->tree = wlr_scene_tree_create(server->layer_tiled);
	view->tree->node.data = &view->node;
	wlr_scene_node_set_enabled(&view->tree->node, false);

	float black[4] = {0, 0, 0, 1};
	view->fs_bg = wlr_scene_rect_create(view->tree, 1, 1, black);
	wlr_scene_node_set_enabled(&view->fs_bg->node, false);
	view->frame = wlr_scene_tree_create(view->tree);
	view->frame_bg = wlr_scene_rect_create(view->frame, 1, 1, black);
	for (int i = 0; i < 4; i++) {
		view->edges[i] = wlr_scene_rect_create(view->frame, 1, 1, black);
		wlr_scene_node_set_enabled(&view->edges[i]->node, false);
	}
	view->titlebar = wlr_scene_buffer_create(view->frame, NULL);
	view->content_tree = wlr_scene_tree_create(view->tree);
	for (int i = 0; i < 4; i++) {
		view->corners[i] = wlr_scene_buffer_create(view->tree, NULL);
		wlr_scene_node_set_enabled(&view->corners[i]->node, false);
	}
	view->popup_tree = wlr_scene_tree_create(server->layer_popups);
	view->popup_tree->node.data = &view->node;
	view->frame_dirty = true;
	view->corners_dirty = true;
	view->workspace = 1;
	wl_list_init(&view->link);
	wl_list_init(&view->focus_link);
	return view;
}

static bool should_float(struct ts_view *view) {
	if (view->type == TS_VIEW_XDG) {
		struct wlr_xdg_toplevel *t = view->xdg_toplevel;
		if (t->parent != NULL) {
			return true;
		}
		if (t->title && strcmp(t->title, "Picture-in-Picture") == 0) {
			return true;
		}
		struct wlr_xdg_toplevel_state *s = &t->current;
		if (s->min_width > 0 && s->min_height > 0 &&
				s->min_width == s->max_width && s->min_height == s->max_height) {
			return true;
		}
	}
#if TESSERA_XWAYLAND
	else if (view->xsurface) {
		struct wlr_xwayland_surface *xs = view->xsurface;
		if (xs->parent != NULL || xs->modal) {
			return true;
		}
		xcb_size_hints_t *h = xs->size_hints;
		if (h && h->min_width > 0 && h->min_width == h->max_width &&
				h->min_height > 0 && h->min_height == h->max_height) {
			return true;
		}
	}
#endif
	return false;
}

static struct ts_view *view_parent(struct ts_view *view) {
	if (view->type == TS_VIEW_XDG && view->xdg_toplevel->parent) {
		return view_from_surface(view->server, view->xdg_toplevel->parent->base->surface);
	}
#if TESSERA_XWAYLAND
	if (view->xsurface && view->xsurface->parent) {
		return view->xsurface->parent->data;
	}
#endif
	return NULL;
}

static bool view_wants_ssd(struct ts_view *view) {
	if (view->type == TS_VIEW_XDG) {
		if (view->decoration) {
			return true;
		}
		struct wlr_server_decoration *deco;
		wl_list_for_each(deco, &view->server->kde_decoration->decorations, link) {
			if (deco->surface == view->surface) {
				return deco->mode == WLR_SERVER_DECORATION_MANAGER_MODE_SERVER;
			}
		}
		return false;
	}
#if TESSERA_XWAYLAND
	if (view->xsurface) {
		return view->xsurface->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;
	}
#endif
	return false;
}

/* Size a new tiled window will most likely get, so its first frame is right. */
static void preview_tiled_size(struct ts_server *server, int *w, int *h) {
	struct ts_output *output = output_focused(server);
	*w = 0;
	*h = 0;
	if (output == NULL) {
		return;
	}
	int g = output_gaps(output);
	struct wlr_box area = output->usable_area;
	int aw = area.width - 2 * g, ah = area.height - 2 * g;
	int tiled = 0;
	struct ts_view *v;
	wl_list_for_each(v, &server->views, link) {
		if (v->mapped && v->output == output && v->workspace == output->workspace &&
				!v->floating && !v->fullscreen && !v->maximized) {
			tiled++;
		}
	}
	if (tiled > 0) {
		aw = (int)((aw - g) * output->master_ratio[output->workspace]);
	}
	*w = aw - 2 * deco_border();
	*h = ah - deco_titlebar_height() - deco_border();
}

void view_map_common(struct ts_view *view) {
	struct ts_server *server = view->server;
	struct ts_output *output = output_focused(server);
	view->output = output;
	view->workspace = output ? output->workspace : 1;
	view->mapped = true;
	view->ssd = view_wants_ssd(view);
	view->floating = should_float(view);
	view->frame_dirty = true;

	if (view->floating) {
		wl_list_insert(server->views.prev, &view->link);
		struct wlr_box geo;
		view_get_geometry(view, &geo);
		int w = MAX(geo.width, 100) + 2 * inset_side(view);
		int h = MAX(geo.height, 60) + inset_top(view) + inset_side(view);
		struct ts_view *parent = view_parent(view);
		struct wlr_box ref = parent && parent->mapped ? parent->box
			: (output ? output->usable_area : (struct wlr_box){0, 0, 800, 600});
		view->float_box = (struct wlr_box){
			ref.x + (ref.width - w) / 2, ref.y + (ref.height - h) / 2, w, h };
		if (parent && parent->mapped) {
			view->workspace = parent->workspace;
			view->output = parent->output;
		}
		view->box = view->float_box;
	} else {
		/* New windows take the big spot. */
		wl_list_insert(&server->views, &view->link);
		view_send_tiled(view, true);
	}
	wl_list_insert(&server->focus_stack, &view->focus_link);

	view_reparent(view);
	wlr_scene_node_set_enabled(&view->tree->node, true);

	bool wants_fullscreen = false;
	if (view->type == TS_VIEW_XDG) {
		wants_fullscreen = view->xdg_toplevel->requested.fullscreen;
	}
#if TESSERA_XWAYLAND
	else if (view->xsurface) {
		wants_fullscreen = view->xsurface->fullscreen;
	}
#endif
	if (wants_fullscreen) {
		view_set_fullscreen(view, true);
	}

	if (output) {
		arrange_output(output);
	} else {
		view_update_frame(view);
	}
	/* A window that opens on a workspace you can't see (a dialog of a
	 * window there) doesn't take the keyboard; its workspace is marked. */
	if (view_is_hidden(view)) {
		view->urgent = true;
	} else {
		view_focus(view);
	}
	cursor_rebase(server);
	ipc_send_state(server);
}

void view_unmap_common(struct ts_view *view) {
	struct ts_server *server = view->server;
	view->mapped = false;
	input_view_gone(view);
	wlr_scene_node_set_enabled(&view->tree->node, false);
	wl_list_remove(&view->link);
	wl_list_init(&view->link);
	wl_list_remove(&view->focus_link);
	wl_list_init(&view->focus_link);
	if (server->focused_view == view) {
		server->focused_view = NULL;
		focus_top_view(server);
	}
	if (view->output) {
		arrange_output(view->output);
	}
	view->surface = NULL;
	view->requested_w = view->requested_h = 0;
	cursor_rebase(server);
	ipc_send_state(server);
}

void view_destroy_common(struct ts_view *view) {
	wl_list_remove(&view->link);
	wl_list_remove(&view->focus_link);
	if (view->server->focused_view == view) {
		view->server->focused_view = NULL;
	}
	input_view_gone(view);
	wlr_scene_node_destroy(&view->popup_tree->node);
	wlr_scene_node_destroy(&view->tree->node);
	free(view);
}

/* ---------------------------------------------------------------------- */
/* xdg-shell                                                                */
/* ---------------------------------------------------------------------- */

static void xdg_map(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, map);
	view->surface = view->xdg_toplevel->base->surface;
	view_map_common(view);
}

static void xdg_unmap(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, unmap);
	view_unmap_common(view);
}

static void xdg_commit(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, commit);
	struct wlr_xdg_toplevel *toplevel = view->xdg_toplevel;
	if (toplevel->base->initial_commit) {
		wlr_xdg_toplevel_set_wm_capabilities(toplevel,
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN |
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE);
		if (view->decoration) {
			wlr_xdg_toplevel_decoration_v1_set_mode(view->decoration,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		}
		if (should_float(view)) {
			wlr_xdg_toplevel_set_size(toplevel, 0, 0);
		} else {
			int w, h;
			preview_tiled_size(view->server, &w, &h);
			view_send_tiled(view, true);
			wlr_xdg_toplevel_set_size(toplevel, MAX(w, 0), MAX(h, 0));
			view->requested_w = w;
			view->requested_h = h;
		}
		return;
	}
	if (!view->mapped) {
		return;
	}
	if (view->floating) {
		view_fit_floating(view);
	} else {
		view_update_frame(view);
	}
}

static void xdg_destroy(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, destroy);
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->request_minimize.link);
	wl_list_remove(&view->set_title.link);
	wl_list_remove(&view->set_app_id.link);
	if (view->decoration) {
		wl_list_remove(&view->decoration_request_mode.link);
		wl_list_remove(&view->decoration_destroy.link);
		view->decoration->data = NULL;
	}
	view_destroy_common(view);
}

static void xdg_request_move(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, request_move);
	input_begin_move(view);
}

static void xdg_request_resize(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, request_resize);
	struct wlr_xdg_toplevel_resize_event *event = data;
	input_begin_resize(view, event->edges);
}

static void xdg_request_maximize(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, request_maximize);
	if (!view->xdg_toplevel->base->initialized) {
		return;
	}
	if (view->mapped) {
		view_set_maximized(view, view->xdg_toplevel->requested.maximized);
	}
	wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
}

static void xdg_request_fullscreen(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, request_fullscreen);
	if (!view->xdg_toplevel->base->initialized) {
		return;
	}
	if (view->mapped) {
		view_set_fullscreen(view, view->xdg_toplevel->requested.fullscreen);
	}
	wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
}

static void xdg_request_minimize(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, request_minimize);
	if (view->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
	}
}

static void xdg_set_title(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, set_title);
	view->frame_dirty = true;
	if (view->mapped) {
		view_update_frame(view);
		if (view->server->focused_view == view) {
			ipc_send_state(view->server);
		}
	}
}

static void xdg_set_app_id(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, set_app_id);
	if (view->mapped && view->server->focused_view == view) {
		ipc_send_state(view->server);
	}
}

static void new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *toplevel = data;

	struct ts_view *view = view_create_common(server, TS_VIEW_XDG);
	if (view == NULL) {
		return;
	}
	view->xdg_toplevel = toplevel;
	wlr_scene_xdg_surface_create(view->content_tree, toplevel->base);
	toplevel->base->data = view->popup_tree;

	view->map.notify = xdg_map;
	wl_signal_add(&toplevel->base->surface->events.map, &view->map);
	view->unmap.notify = xdg_unmap;
	wl_signal_add(&toplevel->base->surface->events.unmap, &view->unmap);
	view->commit.notify = xdg_commit;
	wl_signal_add(&toplevel->base->surface->events.commit, &view->commit);
	view->destroy.notify = xdg_destroy;
	wl_signal_add(&toplevel->events.destroy, &view->destroy);
	view->request_move.notify = xdg_request_move;
	wl_signal_add(&toplevel->events.request_move, &view->request_move);
	view->request_resize.notify = xdg_request_resize;
	wl_signal_add(&toplevel->events.request_resize, &view->request_resize);
	view->request_maximize.notify = xdg_request_maximize;
	wl_signal_add(&toplevel->events.request_maximize, &view->request_maximize);
	view->request_fullscreen.notify = xdg_request_fullscreen;
	wl_signal_add(&toplevel->events.request_fullscreen, &view->request_fullscreen);
	view->request_minimize.notify = xdg_request_minimize;
	wl_signal_add(&toplevel->events.request_minimize, &view->request_minimize);
	view->set_title.notify = xdg_set_title;
	wl_signal_add(&toplevel->events.set_title, &view->set_title);
	view->set_app_id.notify = xdg_set_app_id;
	wl_signal_add(&toplevel->events.set_app_id, &view->set_app_id);
}

/* ---------------------------------------------------------------------- */
/* Popups                                                                   */
/* ---------------------------------------------------------------------- */

static struct wlr_scene_tree *popup_root(struct ts_server *server, struct wlr_scene_tree *tree) {
	while (tree && tree->node.parent && tree->node.parent != server->layer_popups) {
		tree = tree->node.parent;
	}
	return tree;
}

static void popup_commit(struct wl_listener *listener, void *data) {
	struct ts_popup *popup = wl_container_of(listener, popup, commit);
	struct wlr_xdg_popup *xdg_popup = popup->xdg_popup;
	if (!xdg_popup->base->initial_commit) {
		return;
	}
	struct wlr_scene_tree *tree = xdg_popup->base->data;
	struct ts_server *server = NULL;
	struct ts_node *owner = NULL;
	struct wlr_scene_tree *root = NULL;
	if (tree && tree->node.parent) {
		/* Find the server through the owner of the root popup tree. */
		struct wlr_scene_tree *t = tree;
		while (t && t->node.data == NULL) {
			t = t->node.parent;
		}
		owner = t ? t->node.data : NULL;
	}
	if (owner && owner->kind == TS_NODE_VIEW) {
		server = ((struct ts_view *)owner)->server;
	} else if (owner && owner->kind == TS_NODE_LAYER) {
		server = ((struct ts_layer_surface *)owner)->server;
	}
	if (server) {
		root = popup_root(server, tree);
		int lx = root->node.x, ly = root->node.y;
		struct ts_output *output = output_at(server, lx, ly);
		if (output == NULL) {
			output = output_focused(server);
		}
		if (output) {
			struct wlr_box box = output->full_area;
			box.x -= lx;
			box.y -= ly;
			wlr_xdg_popup_unconstrain_from_box(xdg_popup, &box);
		}
	}
	wlr_xdg_surface_schedule_configure(xdg_popup->base);
}

static void popup_destroy(struct wl_listener *listener, void *data) {
	struct ts_popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

void popup_create(struct wlr_xdg_popup *xdg_popup, struct wlr_scene_tree *parent_tree) {
	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

	struct ts_popup *popup = calloc(1, sizeof(*popup));
	if (popup == NULL) {
		wlr_log(WLR_ERROR, "Out of memory");
		return;
	}
	popup->xdg_popup = xdg_popup;
	popup->commit.notify = popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->destroy.notify = popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

static void new_xdg_popup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *xdg_popup = data;
	/* Popups of panels and menus (layer surfaces) start without a parent;
	 * they are set up when the layer surface claims them (layer.c). */
	if (xdg_popup->parent == NULL) {
		return;
	}
	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	if (parent == NULL || parent->data == NULL) {
		return;
	}
	popup_create(xdg_popup, parent->data);
}

/* ---------------------------------------------------------------------- */
/* Decorations                                                              */
/* ---------------------------------------------------------------------- */

static void decoration_request_mode(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, decoration_request_mode);
	if (view->xdg_toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(view->decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

static void decoration_destroy(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, decoration_destroy);
	wl_list_remove(&view->decoration_request_mode.link);
	wl_list_remove(&view->decoration_destroy.link);
	view->decoration = NULL;
	if (view->mapped && view->ssd) {
		view->ssd = false;
		view->frame_dirty = true;
		if (view->output) {
			arrange_output(view->output);
		}
	}
}

static void new_decoration(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;
	struct wlr_scene_tree *tree = deco->toplevel->base->data;
	if (tree == NULL || tree->node.data == NULL) {
		return;
	}
	struct ts_view *view = (struct ts_view *)tree->node.data;
	view->decoration = deco;
	deco->data = view;
	view->decoration_request_mode.notify = decoration_request_mode;
	wl_signal_add(&deco->events.request_mode, &view->decoration_request_mode);
	view->decoration_destroy.notify = decoration_destroy;
	wl_signal_add(&deco->events.destroy, &view->decoration_destroy);
	if (deco->toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(deco,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	if (view->mapped && !view->ssd) {
		view->ssd = true;
		view->frame_dirty = true;
		if (view->output) {
			arrange_output(view->output);
		}
	}
}

void views_init(struct ts_server *server) {
	wl_list_init(&server->views);
	wl_list_init(&server->focus_stack);
	server->xdg_shell = wlr_xdg_shell_create(server->display, 6);
	server->new_xdg_toplevel.notify = new_xdg_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);
	server->new_xdg_popup.notify = new_xdg_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);

	server->xdg_decoration = wlr_xdg_decoration_manager_v1_create(server->display);
	server->new_decoration.notify = new_decoration;
	wl_signal_add(&server->xdg_decoration->events.new_toplevel_decoration,
		&server->new_decoration);
}
