#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include <xcb/xcb.h>
#include "server.h"

/* Support for older X11 apps through Xwayland. Normal windows become
 * views like any other; menus and tooltips ("unmanaged" windows) are
 * shown exactly where the app puts them. */

struct unmanaged {
	struct ts_server *server;
	struct wlr_xwayland_surface *xsurface;
	struct wlr_scene_tree *tree;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener request_configure;
	struct wl_listener set_geometry;
	struct wl_listener set_override_redirect;
};

static struct ts_view *create_view(struct ts_server *server, struct wlr_xwayland_surface *xs);
static void xv_associate(struct wl_listener *listener, void *data);
static void xv_map(struct wl_listener *listener, void *data);

/* ---------------------------- unmanaged ---------------------------- */

static void um_set_geometry(struct wl_listener *listener, void *data) {
	struct unmanaged *um = wl_container_of(listener, um, set_geometry);
	if (um->tree) {
		wlr_scene_node_set_position(&um->tree->node, um->xsurface->x, um->xsurface->y);
	}
}

static void um_map(struct wl_listener *listener, void *data) {
	struct unmanaged *um = wl_container_of(listener, um, map);
	struct wlr_xwayland_surface *xs = um->xsurface;
	um->tree = wlr_scene_subsurface_tree_create(um->server->layer_popups, xs->surface);
	wlr_scene_node_set_position(&um->tree->node, xs->x, xs->y);
	um->set_geometry.notify = um_set_geometry;
	wl_signal_add(&xs->events.set_geometry, &um->set_geometry);
	if (wlr_xwayland_or_surface_wants_focus(xs)) {
		seat_focus_surface(um->server, xs->surface);
	}
}

static void um_unmap(struct wl_listener *listener, void *data) {
	struct unmanaged *um = wl_container_of(listener, um, unmap);
	wl_list_remove(&um->set_geometry.link);
	if (um->tree) {
		wlr_scene_node_destroy(&um->tree->node);
		um->tree = NULL;
	}
	struct ts_server *server = um->server;
	if (server->seat->keyboard_state.focused_surface == um->xsurface->surface) {
		if (um->xsurface->parent && um->xsurface->parent->surface) {
			seat_focus_surface(server, um->xsurface->parent->surface);
		} else if (server->focused_view) {
			seat_focus_surface(server, server->focused_view->surface);
		}
	}
}

static void um_associate(struct wl_listener *listener, void *data) {
	struct unmanaged *um = wl_container_of(listener, um, associate);
	um->map.notify = um_map;
	wl_signal_add(&um->xsurface->surface->events.map, &um->map);
	um->unmap.notify = um_unmap;
	wl_signal_add(&um->xsurface->surface->events.unmap, &um->unmap);
}

static void um_dissociate(struct wl_listener *listener, void *data) {
	struct unmanaged *um = wl_container_of(listener, um, dissociate);
	wl_list_remove(&um->map.link);
	wl_list_remove(&um->unmap.link);
}

static void um_request_configure(struct wl_listener *listener, void *data) {
	struct unmanaged *um = wl_container_of(listener, um, request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;
	wlr_xwayland_surface_configure(um->xsurface, ev->x, ev->y, ev->width, ev->height);
}

static void um_destroy(struct wl_listener *listener, void *data) {
	struct unmanaged *um = wl_container_of(listener, um, destroy);
	wl_list_remove(&um->associate.link);
	wl_list_remove(&um->dissociate.link);
	wl_list_remove(&um->destroy.link);
	wl_list_remove(&um->request_configure.link);
	wl_list_remove(&um->set_override_redirect.link);
	free(um);
}

static void um_set_override_redirect(struct wl_listener *listener, void *data) {
	struct unmanaged *um = wl_container_of(listener, um, set_override_redirect);
	struct wlr_xwayland_surface *xs = um->xsurface;
	struct ts_server *server = um->server;
	bool associated = xs->surface != NULL;
	bool mapped = associated && xs->surface->mapped;
	if (mapped) {
		um_unmap(&um->unmap, NULL);
	}
	if (associated) {
		um_dissociate(&um->dissociate, NULL);
	}
	um_destroy(&um->destroy, NULL);
	struct ts_view *view = create_view(server, xs);
	if (associated) {
		xv_associate(&view->associate, NULL);
		if (mapped) {
			xv_map(&view->map, NULL);
		}
	}
}

static struct unmanaged *create_unmanaged(struct ts_server *server, struct wlr_xwayland_surface *xs) {
	struct unmanaged *um = calloc(1, sizeof(*um));
	if (um == NULL) {
		wlr_log(WLR_ERROR, "Out of memory");
		return NULL;
	}
	um->server = server;
	um->xsurface = xs;
	um->associate.notify = um_associate;
	wl_signal_add(&xs->events.associate, &um->associate);
	um->dissociate.notify = um_dissociate;
	wl_signal_add(&xs->events.dissociate, &um->dissociate);
	um->destroy.notify = um_destroy;
	wl_signal_add(&xs->events.destroy, &um->destroy);
	um->request_configure.notify = um_request_configure;
	wl_signal_add(&xs->events.request_configure, &um->request_configure);
	um->set_override_redirect.notify = um_set_override_redirect;
	wl_signal_add(&xs->events.set_override_redirect, &um->set_override_redirect);
	return um;
}

/* ------------------------------ views ------------------------------ */

static void xv_map(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, map);
	struct wlr_xwayland_surface *xs = view->xsurface;
	view->surface = xs->surface;
	wlr_scene_subsurface_tree_create(view->content_tree, xs->surface);
	view_map_common(view);
	if (view->floating) {
		/* X11 apps place themselves; centre them on screen instead. */
		view_set_box(view, view->box);
	}
}

static void xv_unmap(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, unmap);
	view_unmap_common(view);
	struct wlr_scene_node *child, *tmp;
	wl_list_for_each_safe(child, tmp, &view->content_tree->children, link) {
		wlr_scene_node_destroy(child);
	}
}

static void xv_commit(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, commit);
	if (!view->mapped) {
		return;
	}
	if (view->floating) {
		view_fit_floating(view);
	} else {
		view_update_frame(view);
	}
}

static void xv_associate(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, associate);
	struct wlr_surface *surface = view->xsurface->surface;
	view->map.notify = xv_map;
	wl_signal_add(&surface->events.map, &view->map);
	view->unmap.notify = xv_unmap;
	wl_signal_add(&surface->events.unmap, &view->unmap);
	view->commit.notify = xv_commit;
	wl_signal_add(&surface->events.commit, &view->commit);
}

static void xv_dissociate(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, dissociate);
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->commit.link);
}

static void xv_request_configure(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;
	if (!view->mapped) {
		wlr_xwayland_surface_configure(view->xsurface, ev->x, ev->y, ev->width, ev->height);
		return;
	}
	if (view->floating && !view->fullscreen && !view->maximized) {
		wlr_xwayland_surface_configure(view->xsurface,
			view->box.x + view->content.x, view->box.y + view->content.y,
			ev->width, ev->height);
		return;
	}
	/* Tiled: remind the app of the size we gave it. */
	view->requested_w = 0;
	view_send_size(view, view->content.width, view->content.height);
}

static void xv_request_move(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, request_move);
	input_begin_move(view);
}

static void xv_request_resize(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, request_resize);
	struct wlr_xwayland_resize_event *ev = data;
	input_begin_resize(view, ev->edges);
}

static void xv_request_maximize(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, request_maximize);
	if (view->mapped) {
		struct wlr_xwayland_surface *xs = view->xsurface;
		view_set_maximized(view, xs->maximized_horz || xs->maximized_vert ? !view->maximized : true);
	}
}

static void xv_request_fullscreen(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, request_fullscreen);
	if (view->mapped) {
		view_set_fullscreen(view, view->xsurface->fullscreen);
	}
}

static void xv_request_minimize(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;
}

static void xv_request_activate(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, request_activate);
	if (view->mapped && view_visible(view)) {
		view_focus(view);
	}
}

static void xv_set_title(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, set_title);
	view->frame_dirty = true;
	if (view->mapped) {
		view_update_frame(view);
		if (view->server->focused_view == view) {
			ipc_send_state(view->server);
		}
	}
}

static void xv_set_class(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, set_app_id);
	if (view->mapped && view->server->focused_view == view) {
		ipc_send_state(view->server);
	}
}

static void xv_set_hints(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, set_hints);
	(void)view;
}

static void xv_set_override_redirect(struct wl_listener *listener, void *data);

static void xv_destroy(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, destroy);
	wl_list_remove(&view->associate.link);
	wl_list_remove(&view->dissociate.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->request_configure.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->request_minimize.link);
	wl_list_remove(&view->request_activate.link);
	wl_list_remove(&view->set_title.link);
	wl_list_remove(&view->set_app_id.link);
	wl_list_remove(&view->set_hints.link);
	wl_list_remove(&view->set_override_redirect.link);
	view->xsurface->data = NULL;
	view_destroy_common(view);
}

static void xv_set_override_redirect(struct wl_listener *listener, void *data) {
	struct ts_view *view = wl_container_of(listener, view, set_override_redirect);
	struct wlr_xwayland_surface *xs = view->xsurface;
	struct ts_server *server = view->server;
	bool associated = xs->surface != NULL;
	bool mapped = associated && view->mapped;
	if (mapped) {
		xv_unmap(&view->unmap, NULL);
	}
	if (associated) {
		xv_dissociate(&view->dissociate, NULL);
	}
	xv_destroy(&view->destroy, NULL);
	struct unmanaged *um = create_unmanaged(server, xs);
	if (um != NULL && associated) {
		um_associate(&um->associate, NULL);
		if (mapped) {
			um_map(&um->map, NULL);
		}
	}
}

static struct ts_view *create_view(struct ts_server *server, struct wlr_xwayland_surface *xs) {
	struct ts_view *view = view_create_common(server, TS_VIEW_XWAYLAND);
	if (view == NULL) {
		return NULL;
	}
	view->xsurface = xs;
	xs->data = view;

	view->associate.notify = xv_associate;
	wl_signal_add(&xs->events.associate, &view->associate);
	view->dissociate.notify = xv_dissociate;
	wl_signal_add(&xs->events.dissociate, &view->dissociate);
	view->destroy.notify = xv_destroy;
	wl_signal_add(&xs->events.destroy, &view->destroy);
	view->request_configure.notify = xv_request_configure;
	wl_signal_add(&xs->events.request_configure, &view->request_configure);
	view->request_move.notify = xv_request_move;
	wl_signal_add(&xs->events.request_move, &view->request_move);
	view->request_resize.notify = xv_request_resize;
	wl_signal_add(&xs->events.request_resize, &view->request_resize);
	view->request_maximize.notify = xv_request_maximize;
	wl_signal_add(&xs->events.request_maximize, &view->request_maximize);
	view->request_fullscreen.notify = xv_request_fullscreen;
	wl_signal_add(&xs->events.request_fullscreen, &view->request_fullscreen);
	view->request_minimize.notify = xv_request_minimize;
	wl_signal_add(&xs->events.request_minimize, &view->request_minimize);
	view->request_activate.notify = xv_request_activate;
	wl_signal_add(&xs->events.request_activate, &view->request_activate);
	view->set_title.notify = xv_set_title;
	wl_signal_add(&xs->events.set_title, &view->set_title);
	view->set_app_id.notify = xv_set_class;
	wl_signal_add(&xs->events.set_class, &view->set_app_id);
	view->set_hints.notify = xv_set_hints;
	wl_signal_add(&xs->events.set_hints, &view->set_hints);
	view->set_override_redirect.notify = xv_set_override_redirect;
	wl_signal_add(&xs->events.set_override_redirect, &view->set_override_redirect);
	return view;
}

static void handle_new_surface(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, new_xwayland_surface);
	struct wlr_xwayland_surface *xs = data;
	if (xs->override_redirect) {
		create_unmanaged(server, xs);
	} else {
		create_view(server, xs);
	}
}

static void handle_ready(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, xwayland_ready);
	wlr_xwayland_set_seat(server->xwayland, server->seat);
	struct wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(server->cursor_mgr,
		"default", 1);
	if (xcursor) {
		struct wlr_xcursor_image *image = xcursor->images[0];
		wlr_xwayland_set_cursor(server->xwayland, image->buffer, image->width * 4,
			image->width, image->height, image->hotspot_x, image->hotspot_y);
	}
}

void xwayland_init(struct ts_server *server) {
	server->xwayland = wlr_xwayland_create(server->display, server->compositor, true);
	if (server->xwayland == NULL) {
		wlr_log(WLR_INFO, "Xwayland is not available; only Wayland apps will run");
		unsetenv("DISPLAY");
		return;
	}
	server->xwayland_ready.notify = handle_ready;
	wl_signal_add(&server->xwayland->events.ready, &server->xwayland_ready);
	server->new_xwayland_surface.notify = handle_new_surface;
	wl_signal_add(&server->xwayland->events.new_surface, &server->new_xwayland_surface);
	setenv("DISPLAY", server->xwayland->display_name, true);
}
