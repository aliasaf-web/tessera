#define _POSIX_C_SOURCE 200809L
#include <libinput.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/backend/libinput.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include "server.h"

static uint32_t now_msec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void notify_activity(struct ts_server *server) {
	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
	idle_reset(server);
}

/* ---------------------------------------------------------------------- */
/* Keyboard                                                                 */
/* ---------------------------------------------------------------------- */

void seat_focus_surface(struct ts_server *server, struct wlr_surface *surface) {
	struct wlr_seat *seat = server->seat;
	if (surface == NULL) {
		wlr_seat_keyboard_notify_clear_focus(seat);
		return;
	}
	if (seat->keyboard_state.focused_surface == surface) {
		return;
	}
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	if (kb) {
		wlr_seat_keyboard_notify_enter(seat, surface, kb->keycodes, kb->num_keycodes,
			&kb->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, NULL);
	}
}

static struct xkb_keymap *make_keymap(struct ts_server *server) {
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct ts_config *c = &server->config;
	struct xkb_rule_names names = {
		.rules = NULL,
		.model = c->kb_model[0] ? c->kb_model : NULL,
		.layout = c->kb_layout[0] ? c->kb_layout : NULL,
		.variant = c->kb_variant[0] ? c->kb_variant : NULL,
		.options = c->kb_options[0] ? c->kb_options : NULL,
	};
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (keymap == NULL) {
		wlr_log(WLR_ERROR, "Keyboard layout '%s' could not be loaded, using the default",
			c->kb_layout);
		keymap = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
	xkb_context_unref(ctx);
	return keymap;
}

void keyboard_apply_config(struct ts_server *server) {
	struct xkb_keymap *keymap = make_keymap(server);
	if (keymap == NULL) {
		return;
	}
	struct ts_keyboard *kb;
	wl_list_for_each(kb, &server->keyboards, link) {
		wlr_keyboard_set_keymap(kb->wlr_keyboard, keymap);
	}
	xkb_keymap_unref(keymap);
}

static void send_key_event(struct ts_server *server, const char *action) {
	char line[128];
	snprintf(line, sizeof(line), "{\"event\":\"key\",\"action\":\"%s\"}", action);
	ipc_broadcast(server, line);
}

static void focus_cycle(struct ts_server *server) {
	/* Bring the least recently used visible window forward. */
	struct ts_view *view, *last = NULL;
	wl_list_for_each(view, &server->focus_stack, focus_link) {
		if (view_visible(view)) {
			last = view;
		}
	}
	if (last && last != server->focused_view) {
		view_focus(last);
	}
}

static bool handle_media_key(struct ts_server *server, xkb_keysym_t sym) {
	switch (sym) {
	case XKB_KEY_XF86AudioRaiseVolume:
		send_key_event(server, "volume-up");
		return true;
	case XKB_KEY_XF86AudioLowerVolume:
		send_key_event(server, "volume-down");
		return true;
	case XKB_KEY_XF86AudioMute:
		send_key_event(server, "volume-mute");
		return true;
	case XKB_KEY_XF86AudioMicMute:
		send_key_event(server, "mic-mute");
		return true;
	case XKB_KEY_XF86MonBrightnessUp:
		send_key_event(server, "brightness-up");
		return true;
	case XKB_KEY_XF86MonBrightnessDown:
		send_key_event(server, "brightness-down");
		return true;
	case XKB_KEY_XF86AudioPlay:
	case XKB_KEY_XF86AudioPause:
		send_key_event(server, "media-play-pause");
		return true;
	case XKB_KEY_XF86AudioNext:
		send_key_event(server, "media-next");
		return true;
	case XKB_KEY_XF86AudioPrev:
		send_key_event(server, "media-previous");
		return true;
	default:
		return false;
	}
}

static bool handle_binding(struct ts_server *server, uint32_t mods, xkb_keysym_t sym) {
	struct ts_view *focused = server->focused_view;
	bool super = mods & WLR_MODIFIER_LOGO;
	bool shift = mods & WLR_MODIFIER_SHIFT;
	bool alt = mods & WLR_MODIFIER_ALT;
	bool ctrl = mods & WLR_MODIFIER_CTRL;

	if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
		if (server->session) {
			wlr_session_change_vt(server->session, sym - XKB_KEY_XF86Switch_VT_1 + 1);
		}
		return true;
	}
	if (handle_media_key(server, sym)) {
		return true;
	}
	if (sym == XKB_KEY_XF86PowerOff) {
		send_key_event(server, "power");
		return true;
	}
	if (sym == XKB_KEY_Print) {
		send_key_event(server, shift ? "screenshot-area" : "screenshot");
		return true;
	}
	if (alt && !super && !ctrl) {
		if (sym == XKB_KEY_F4) {
			view_close(focused);
			return true;
		}
		if (sym == XKB_KEY_Tab) {
			focus_cycle(server);
			return true;
		}
	}
	if (!super) {
		return false;
	}

	if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
		int ws = sym - XKB_KEY_1 + 1;
		if (shift) {
			if (focused) {
				view_move_to_workspace(focused, ws);
			}
		} else {
			workspace_switch(server, ws);
		}
		return true;
	}

	int dx = 0, dy = 0;
	switch (sym) {
	case XKB_KEY_Left:
		dx = -1;
		break;
	case XKB_KEY_Right:
		dx = 1;
		break;
	case XKB_KEY_Up:
		dy = -1;
		break;
	case XKB_KEY_Down:
		dy = 1;
		break;
	default:
		break;
	}
	if (dx || dy) {
		struct ts_view *target = view_in_direction(server, focused, dx, dy);
		if (target) {
			if (shift && !focused->floating && !target->floating) {
				view_swap(focused, target);
				view_focus(focused);
			} else if (!shift) {
				view_focus(target);
			}
		}
		return true;
	}

	switch (sym) {
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		server_spawn(server->config.terminal);
		return true;
	case XKB_KEY_e:
		server_spawn(server->config.files);
		return true;
	case XKB_KEY_b:
		server_spawn(server->config.browser);
		return true;
	case XKB_KEY_a:
		send_key_event(server, "launcher");
		return true;
	case XKB_KEY_q:
	case XKB_KEY_w:
		view_close(focused);
		return true;
	case XKB_KEY_f:
		if (focused) {
			view_set_fullscreen(focused, !focused->fullscreen);
		}
		return true;
	case XKB_KEY_m:
		if (focused && !focused->fullscreen) {
			view_set_maximized(focused, !focused->maximized);
		}
		return true;
	case XKB_KEY_space:
		if (focused && !focused->fullscreen) {
			view_set_floating(focused, !focused->floating);
		}
		return true;
	case XKB_KEY_Tab:
		focus_cycle(server);
		return true;
	case XKB_KEY_l:
		lock_engage(server);
		return true;
	case XKB_KEY_s:
		if (shift) {
			send_key_event(server, "screenshot-area");
			return true;
		}
		return false;
	case XKB_KEY_r:
	case XKB_KEY_R:
		if (shift) {
			config_reload(server);
			return true;
		}
		return false;
	case XKB_KEY_slash:
	case XKB_KEY_question:
		send_key_event(server, "help");
		return true;
	case XKB_KEY_E:
	case XKB_KEY_Escape:
		if (shift || sym == XKB_KEY_Escape) {
			send_key_event(server, "power");
			return true;
		}
		return false;
	default:
		return false;
	}
}

static bool is_super(xkb_keysym_t sym) {
	return sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct ts_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct ts_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;
	struct xkb_state *state = keyboard->wlr_keyboard->xkb_state;
	uint32_t keycode = event->keycode + 8;
	bool pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;

	notify_activity(server);

	/* Translated symbols (with Shift etc. applied) and the plain symbols
	 * of the first shift level, so Super+Shift+1 still means "1". */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(state, keycode, &syms);
	const xkb_keysym_t *raw_syms;
	xkb_layout_index_t layout = xkb_state_key_get_layout(state, keycode);
	int nraw = xkb_keymap_key_get_syms_by_level(keyboard->wlr_keyboard->keymap,
		keycode, layout, 0, &raw_syms);
	uint32_t mods = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);

	if (server->locked) {
		if (pressed) {
			bool done = false;
			for (int i = 0; i < nsyms && !done; i++) {
				if ((syms[i] >= XKB_KEY_XF86Switch_VT_1 && syms[i] <= XKB_KEY_XF86Switch_VT_12) ||
						handle_media_key(server, syms[i])) {
					if (syms[i] >= XKB_KEY_XF86Switch_VT_1 && syms[i] <= XKB_KEY_XF86Switch_VT_12 &&
							server->session) {
						wlr_session_change_vt(server->session,
							syms[i] - XKB_KEY_XF86Switch_VT_1 + 1);
					}
					done = true;
				}
			}
			if (!done) {
				xkb_keysym_t sym = xkb_state_key_get_one_sym(state, keycode);
				uint32_t cp = xkb_state_key_get_utf32(state, keycode);
				lock_handle_key(server, sym, cp);
			}
		}
		return;
	}

	bool handled = false;
	if (pressed) {
		bool super_key = false;
		for (int i = 0; i < nraw; i++) {
			if (is_super(raw_syms[i])) {
				super_key = true;
			}
		}
		if (super_key) {
			server->super_alone = (mods & ~WLR_MODIFIER_LOGO) == 0;
			server->super_keycode = keycode;
		} else {
			server->super_alone = false;
		}

		for (int i = 0; i < nsyms && !handled; i++) {
			handled = handle_binding(server, mods, syms[i]);
		}
		for (int i = 0; i < nraw && !handled; i++) {
			handled = handle_binding(server, mods, raw_syms[i]);
		}
	} else if (keycode == server->super_keycode) {
		if (server->super_alone) {
			send_key_event(server, "launcher");
		}
		server->super_alone = false;
		server->super_keycode = 0;
	}

	if (!handled) {
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
	}
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
	struct ts_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

static void update_capabilities(struct ts_server *server);

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct ts_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
	struct ts_server *server = keyboard->server;
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
	update_capabilities(server);
}

static void new_keyboard_ex(struct ts_server *server, struct wlr_input_device *device,
		bool set_keymap) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
	struct ts_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	if (set_keymap) {
		struct xkb_keymap *keymap = make_keymap(server);
		if (keymap) {
			wlr_keyboard_set_keymap(wlr_keyboard, keymap);
			xkb_keymap_unref(keymap);
		}
		wlr_keyboard_set_repeat_info(wlr_keyboard, 30, 400);
	}

	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, wlr_keyboard);
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void new_keyboard(struct ts_server *server, struct wlr_input_device *device) {
	new_keyboard_ex(server, device, true);
}

static void update_capabilities(struct ts_server *server) {
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

/* On-screen keyboards and similar tools. */
static struct wl_listener new_virtual_keyboard_listener;
static struct wl_listener new_virtual_pointer_listener;
static struct ts_server *virtual_server;

static void new_virtual_keyboard(struct wl_listener *listener, void *data) {
	struct wlr_virtual_keyboard_v1 *vkbd = data;
	new_keyboard_ex(virtual_server, &vkbd->keyboard.base, false);
	update_capabilities(virtual_server);
}

static void new_virtual_pointer(struct wl_listener *listener, void *data) {
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	wlr_cursor_attach_input_device(virtual_server->cursor, &event->new_pointer->pointer.base);
	update_capabilities(virtual_server);
}

/* ---------------------------------------------------------------------- */
/* Pointer                                                                  */
/* ---------------------------------------------------------------------- */

static void configure_libinput(struct ts_server *server, struct wlr_input_device *device) {
	if (!wlr_input_device_is_libinput(device)) {
		return;
	}
	struct libinput_device *li = wlr_libinput_get_device_handle(device);
	if (libinput_device_config_tap_get_finger_count(li) > 0) {
		libinput_device_config_tap_set_enabled(li, server->config.tap_to_click ?
			LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
		libinput_device_config_tap_set_drag_enabled(li, LIBINPUT_CONFIG_DRAG_ENABLED);
		if (libinput_device_config_dwt_is_available(li)) {
			libinput_device_config_dwt_set_enabled(li, LIBINPUT_CONFIG_DWT_ENABLED);
		}
	}
	if (libinput_device_config_scroll_has_natural_scroll(li)) {
		libinput_device_config_scroll_set_natural_scroll_enabled(li,
			server->config.natural_scroll);
	}
	if (libinput_device_config_middle_emulation_is_available(li)) {
		libinput_device_config_middle_emulation_set_enabled(li,
			LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED);
	}
}

static void new_pointer(struct ts_server *server, struct wlr_input_device *device) {
	configure_libinput(server, device);
	wlr_cursor_attach_input_device(server->cursor, device);
}

static void new_input(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
	case WLR_INPUT_DEVICE_TOUCH:
	case WLR_INPUT_DEVICE_TABLET:
		new_pointer(server, device);
		break;
	default:
		break;
	}
	update_capabilities(server);
}

static void set_cursor_image(struct ts_server *server, const char *name) {
	if (server->cursor_image == name) {
		return;
	}
	server->cursor_image = name;
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, name);
}

/* What is under the cursor? */
struct hit {
	struct ts_view *view;
	struct ts_layer_surface *layer;
	struct wlr_surface *surface;
	double sx, sy;
	bool on_frame;
	double fx, fy; /* position on the frame */
};

static struct hit hit_test(struct ts_server *server, double lx, double ly) {
	struct hit hit = {0};
	double nx, ny;
	struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, &nx, &ny);
	if (node == NULL) {
		return hit;
	}
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(buffer);
		if (ss) {
			hit.surface = ss->surface;
			hit.sx = nx;
			hit.sy = ny;
		}
	}
	struct wlr_scene_tree *tree = node->parent;
	struct ts_node *owner = node->data;
	while (owner == NULL && tree != NULL) {
		owner = tree->node.data;
		tree = tree->node.parent;
	}
	if (owner == NULL) {
		return hit;
	}
	if (owner->kind == TS_NODE_VIEW) {
		hit.view = (struct ts_view *)owner;
		if (hit.surface == NULL) {
			hit.on_frame = true;
			hit.fx = lx - hit.view->box.x;
			hit.fy = ly - hit.view->box.y;
		}
	} else if (owner->kind == TS_NODE_LAYER) {
		hit.layer = (struct ts_layer_surface *)owner;
	}
	return hit;
}

static void set_hover_close(struct ts_server *server, struct ts_view *view) {
	if (server->hover_view == view) {
		return;
	}
	struct ts_view *old = server->hover_view;
	server->hover_view = view;
	if (old) {
		old->frame_hover_close = false;
		old->frame_dirty = true;
		view_update_frame(old);
	}
	if (view) {
		view->frame_hover_close = true;
		view->frame_dirty = true;
		view_update_frame(view);
	}
}

static void update_drop_hint(struct ts_server *server, struct ts_view *target) {
	server->swap_target = target;
	if (target == NULL) {
		wlr_scene_node_set_enabled(&server->drop_hint->node, false);
		return;
	}
	struct ts_color a = server->theme.accent;
	float color[4] = { a.r * 0.3f, a.g * 0.3f, a.b * 0.3f, 0.3f };
	wlr_scene_rect_set_color(server->drop_hint, color);
	wlr_scene_rect_set_size(server->drop_hint, target->box.width, target->box.height);
	wlr_scene_node_set_position(&server->drop_hint->node, target->box.x, target->box.y);
	wlr_scene_node_set_enabled(&server->drop_hint->node, true);
}

static void process_motion(struct ts_server *server, uint32_t time) {
	double lx = server->cursor->x, ly = server->cursor->y;
	struct wlr_seat *seat = server->seat;
	wlr_scene_node_set_position(&server->drag_icons->node, lx, ly);

	switch (server->cursor_mode) {
	case TS_CURSOR_MOVE: {
		struct ts_view *view = server->grab_view;
		view->box.x = lx - server->grab_x;
		view->box.y = ly - server->grab_y;
		view->float_box = view->box;
		view_update_frame(view);
		struct ts_output *output = output_at(server, lx, ly);
		if (output && output != view->output) {
			view->output = output;
			view->workspace = output->workspace;
		}
		return;
	}
	case TS_CURSOR_RESIZE: {
		struct ts_view *view = server->grab_view;
		struct wlr_box b = server->grab_box;
		double dx = lx - server->grab_x, dy = ly - server->grab_y;
		int left = b.x, right = b.x + b.width, top = b.y, bottom = b.y + b.height;
		if (server->resize_edges & WLR_EDGE_LEFT) {
			left += dx;
		}
		if (server->resize_edges & WLR_EDGE_RIGHT) {
			right += dx;
		}
		if (server->resize_edges & WLR_EDGE_TOP) {
			top += dy;
		}
		if (server->resize_edges & WLR_EDGE_BOTTOM) {
			bottom += dy;
		}
		if (right - left < 160) {
			if (server->resize_edges & WLR_EDGE_LEFT) {
				left = right - 160;
			} else {
				right = left + 160;
			}
		}
		if (bottom - top < 100) {
			if (server->resize_edges & WLR_EDGE_TOP) {
				top = bottom - 100;
			} else {
				bottom = top + 100;
			}
		}
		struct wlr_box nb = { left, top, right - left, bottom - top };
		view->float_box = nb;
		view_set_box(view, nb);
		return;
	}
	case TS_CURSOR_SWAP: {
		struct hit hit = hit_test(server, lx, ly);
		struct ts_view *target = NULL;
		if (hit.view && hit.view != server->grab_view && !hit.view->floating &&
				!hit.view->fullscreen && !hit.view->maximized) {
			target = hit.view;
		}
		update_drop_hint(server, target);
		set_cursor_image(server, "grabbing");
		return;
	}
	case TS_CURSOR_RATIO: {
		if (server->ratio_output) {
			layout_set_ratio_from_cursor(server->ratio_output, lx);
		}
		return;
	}
	case TS_CURSOR_CLOSE_PRESS:
		return;
	case TS_CURSOR_PASSTHROUGH:
		break;
	}

	if (server->locked) {
		wlr_seat_pointer_clear_focus(seat);
		set_cursor_image(server, "default");
		return;
	}

	struct hit hit = hit_test(server, lx, ly);
	if (hit.on_frame && hit.view->ssd && hit.fy < deco_titlebar_height() &&
			deco_close_hit(hit.view->box.width, hit.fx, hit.fy)) {
		set_hover_close(server, hit.view);
	} else {
		set_hover_close(server, NULL);
	}

	if (hit.surface) {
		wlr_seat_pointer_notify_enter(seat, hit.surface, hit.sx, hit.sy);
		wlr_seat_pointer_notify_motion(seat, time, hit.sx, hit.sy);
		/* The client sets its own cursor image. */
		server->cursor_image = NULL;
		return;
	}
	wlr_seat_pointer_clear_focus(seat);
	struct ts_output *output = output_at(server, lx, ly);
	if (!hit.view && !hit.layer && layout_gap_hit(output, lx, ly)) {
		set_cursor_image(server, "col-resize");
	} else {
		set_cursor_image(server, "default");
	}
}

void cursor_rebase(struct ts_server *server) {
	if (server->cursor_mode == TS_CURSOR_PASSTHROUGH) {
		process_motion(server, now_msec());
	}
}

static void cursor_motion(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	notify_activity(server);
	wlr_relative_pointer_manager_v1_send_relative_motion(server->relative_pointer,
		server->seat, (uint64_t)event->time_msec * 1000, event->delta_x, event->delta_y,
		event->unaccel_dx, event->unaccel_dy);
	wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
	process_motion(server, event->time_msec);
}

static void cursor_motion_absolute(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	notify_activity(server);
	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(server->cursor, &event->pointer->base,
		event->x, event->y, &lx, &ly);
	double dx = lx - server->cursor->x, dy = ly - server->cursor->y;
	wlr_relative_pointer_manager_v1_send_relative_motion(server->relative_pointer,
		server->seat, (uint64_t)event->time_msec * 1000, dx, dy, dx, dy);
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
	process_motion(server, event->time_msec);
}

static void end_grab(struct ts_server *server) {
	if (server->cursor_mode == TS_CURSOR_SWAP) {
		struct ts_view *target = server->swap_target;
		struct ts_view *view = server->grab_view;
		update_drop_hint(server, NULL);
		if (target && view) {
			view_swap(view, target);
			view_focus(view);
		}
	}
	server->cursor_mode = TS_CURSOR_PASSTHROUGH;
	server->grab_view = NULL;
	server->ratio_output = NULL;
	server->cursor_image = NULL;
	cursor_rebase(server);
}

void input_view_gone(struct ts_view *view) {
	struct ts_server *server = view->server;
	if (server->grab_view == view) {
		update_drop_hint(server, NULL);
		server->cursor_mode = TS_CURSOR_PASSTHROUGH;
		server->grab_view = NULL;
	}
	if (server->swap_target == view) {
		update_drop_hint(server, NULL);
	}
	if (server->hover_view == view) {
		server->hover_view = NULL;
	}
}

void input_begin_move(struct ts_view *view) {
	struct ts_server *server = view->server;
	if (!view->mapped || view->fullscreen || server->locked ||
			server->cursor_mode != TS_CURSOR_PASSTHROUGH) {
		return;
	}
	server->grab_view = view;
	if (view->floating) {
		server->cursor_mode = TS_CURSOR_MOVE;
		server->grab_x = server->cursor->x - view->box.x;
		server->grab_y = server->cursor->y - view->box.y;
		set_cursor_image(server, "grabbing");
	} else if (!view->maximized) {
		server->cursor_mode = TS_CURSOR_SWAP;
		server->swap_target = NULL;
	} else {
		server->grab_view = NULL;
		return;
	}
	wlr_seat_pointer_clear_focus(server->seat);
}

void input_begin_resize(struct ts_view *view, uint32_t edges) {
	struct ts_server *server = view->server;
	if (!view->mapped || !view->floating || view->fullscreen || view->maximized ||
			server->locked || server->cursor_mode != TS_CURSOR_PASSTHROUGH) {
		return;
	}
	server->grab_view = view;
	server->cursor_mode = TS_CURSOR_RESIZE;
	server->grab_x = server->cursor->x;
	server->grab_y = server->cursor->y;
	server->grab_box = view->box;
	server->resize_edges = edges;
	wlr_seat_pointer_clear_focus(server->seat);
}

static uint32_t edges_for_point(struct ts_view *view, double lx, double ly) {
	uint32_t edges = 0;
	double cx = view->box.x + view->box.width / 2.0;
	double cy = view->box.y + view->box.height / 2.0;
	edges |= lx < cx ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
	edges |= ly < cy ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
	return edges;
}

static void cursor_button(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	notify_activity(server);
	server->super_alone = false;

	bool pressed = event->state == WL_POINTER_BUTTON_STATE_PRESSED;
	if (server->locked) {
		return;
	}

	if (!pressed && server->cursor_mode != TS_CURSOR_PASSTHROUGH) {
		if (server->cursor_mode == TS_CURSOR_CLOSE_PRESS) {
			struct hit hit = hit_test(server, server->cursor->x, server->cursor->y);
			if (hit.view == server->grab_view && hit.on_frame &&
					deco_close_hit(hit.view->box.width, hit.fx, hit.fy)) {
				view_close(hit.view);
			}
		}
		end_grab(server);
		return;
	}

	double lx = server->cursor->x, ly = server->cursor->y;
	struct hit hit = hit_test(server, lx, ly);

	if (pressed) {
		struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
		bool super = kb && (wlr_keyboard_get_modifiers(kb) & WLR_MODIFIER_LOGO);

		if (hit.view && super) {
			view_focus(hit.view);
			if (event->button == BTN_LEFT) {
				input_begin_move(hit.view);
			} else if (event->button == BTN_RIGHT && hit.view->floating) {
				input_begin_resize(hit.view, edges_for_point(hit.view, lx, ly));
			}
			return;
		}

		if (hit.view && hit.on_frame) {
			view_focus(hit.view);
			if (event->button != BTN_LEFT) {
				return;
			}
			if (hit.view->ssd && hit.fy < deco_titlebar_height() &&
					deco_close_hit(hit.view->box.width, hit.fx, hit.fy)) {
				server->cursor_mode = TS_CURSOR_CLOSE_PRESS;
				server->grab_view = hit.view;
				return;
			}
			static uint32_t last_click;
			static struct ts_view *last_view;
			if (hit.fy < deco_titlebar_height() && last_view == hit.view &&
					event->time_msec - last_click < 400) {
				last_view = NULL;
				view_set_maximized(hit.view, !hit.view->maximized);
				return;
			}
			last_click = event->time_msec;
			last_view = hit.view;
			if (hit.view->floating) {
				/* Edges of a floating window resize it. */
				int edge = 8;
				uint32_t edges = 0;
				if (hit.fx < edge) edges |= WLR_EDGE_LEFT;
				if (hit.fx > hit.view->box.width - edge) edges |= WLR_EDGE_RIGHT;
				if (hit.fy > hit.view->box.height - edge) edges |= WLR_EDGE_BOTTOM;
				if (edges) {
					input_begin_resize(hit.view, edges);
					return;
				}
			}
			input_begin_move(hit.view);
			return;
		}

		if (!hit.view && !hit.layer && event->button == BTN_LEFT) {
			struct ts_output *output = output_at(server, lx, ly);
			if (layout_gap_hit(output, lx, ly)) {
				server->cursor_mode = TS_CURSOR_RATIO;
				server->ratio_output = output;
				return;
			}
		}

		if (hit.view) {
			view_focus(hit.view);
		} else if (hit.layer && hit.layer->layer_surface->current.keyboard_interactive !=
				ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
			server->focused_layer = hit.layer;
			seat_focus_surface(server, hit.layer->layer_surface->surface);
		}
	}

	wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
}

static void cursor_axis(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	notify_activity(server);
	if (server->locked) {
		return;
	}
	wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation,
		event->delta, event->delta_discrete, event->source, event->relative_direction);
}

static void cursor_frame(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

/* ---------------------------------------------------------------------- */
/* Seat requests                                                            */
/* ---------------------------------------------------------------------- */

static void request_cursor(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	if (server->cursor_mode != TS_CURSOR_PASSTHROUGH || server->locked) {
		return;
	}
	if (server->seat->pointer_state.focused_client == event->seat_client) {
		server->cursor_image = NULL;
		wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
	}
}

static void request_set_shape(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, request_set_shape);
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (server->cursor_mode != TS_CURSOR_PASSTHROUGH || server->locked) {
		return;
	}
	if (server->seat->pointer_state.focused_client == event->seat_client) {
		server->cursor_image = NULL;
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
			wlr_cursor_shape_v1_name(event->shape));
	}
}

static void request_set_selection(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void request_set_primary_selection(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

static void request_start_drag(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, request_start_drag);
	struct wlr_seat_request_start_drag_event *event = data;
	if (wlr_seat_validate_pointer_grab_serial(server->seat, event->origin, event->serial)) {
		wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
	} else {
		wlr_data_source_destroy(event->drag->source);
	}
}

struct drag_icon {
	struct wlr_scene_tree *tree;
	struct wl_listener destroy;
};

static void drag_icon_destroy(struct wl_listener *listener, void *data) {
	struct drag_icon *icon = wl_container_of(listener, icon, destroy);
	wl_list_remove(&icon->destroy.link);
	wlr_scene_node_destroy(&icon->tree->node);
	free(icon);
}

static void start_drag(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, start_drag);
	struct wlr_drag *drag = data;
	if (drag->icon == NULL) {
		return;
	}
	struct drag_icon *icon = calloc(1, sizeof(*icon));
	icon->tree = wlr_scene_drag_icon_create(server->drag_icons, drag->icon);
	icon->destroy.notify = drag_icon_destroy;
	wl_signal_add(&drag->icon->events.destroy, &icon->destroy);
	drag->icon->data = icon->tree;
}


void input_init(struct ts_server *server) {
	server->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
	const char *theme = getenv("XCURSOR_THEME");
	const char *size_env = getenv("XCURSOR_SIZE");
	int size = size_env ? atoi(size_env) : 24;
	if (size <= 0) {
		size = 24;
	}
	server->cursor_mgr = wlr_xcursor_manager_create(theme, size);
	wlr_xcursor_manager_load(server->cursor_mgr, 1);

	server->cursor_mode = TS_CURSOR_PASSTHROUGH;
	server->cursor_motion.notify = cursor_motion;
	wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
	server->cursor_motion_absolute.notify = cursor_motion_absolute;
	wl_signal_add(&server->cursor->events.motion_absolute, &server->cursor_motion_absolute);
	server->cursor_button.notify = cursor_button;
	wl_signal_add(&server->cursor->events.button, &server->cursor_button);
	server->cursor_axis.notify = cursor_axis;
	wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
	server->cursor_frame.notify = cursor_frame;
	wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);

	wl_list_init(&server->keyboards);
	server->new_input.notify = new_input;
	wl_signal_add(&server->backend->events.new_input, &server->new_input);
	server->seat = wlr_seat_create(server->display, "seat0");
	/* There is always a cursor, even before a mouse is plugged in. */
	wlr_seat_set_capabilities(server->seat, WL_SEAT_CAPABILITY_POINTER);
	server->request_cursor.notify = request_cursor;
	wl_signal_add(&server->seat->events.request_set_cursor, &server->request_cursor);
	server->request_set_selection.notify = request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection, &server->request_set_selection);
	server->request_set_primary_selection.notify = request_set_primary_selection;
	wl_signal_add(&server->seat->events.request_set_primary_selection,
		&server->request_set_primary_selection);
	server->request_start_drag.notify = request_start_drag;
	wl_signal_add(&server->seat->events.request_start_drag, &server->request_start_drag);
	server->start_drag.notify = start_drag;
	wl_signal_add(&server->seat->events.start_drag, &server->start_drag);

	server->relative_pointer = wlr_relative_pointer_manager_v1_create(server->display);
	server->cursor_shape = wlr_cursor_shape_manager_v1_create(server->display, 1);
	server->request_set_shape.notify = request_set_shape;
	wl_signal_add(&server->cursor_shape->events.request_set_shape, &server->request_set_shape);

	float none[4] = {0, 0, 0, 0};
	server->drop_hint = wlr_scene_rect_create(server->layer_drag, 1, 1, none);
	wlr_scene_node_set_enabled(&server->drop_hint->node, false);
	server->drag_icons = wlr_scene_tree_create(server->layer_drag);

	virtual_server = server;
	struct wlr_virtual_keyboard_manager_v1 *vk =
		wlr_virtual_keyboard_manager_v1_create(server->display);
	new_virtual_keyboard_listener.notify = new_virtual_keyboard;
	wl_signal_add(&vk->events.new_virtual_keyboard, &new_virtual_keyboard_listener);
	struct wlr_virtual_pointer_manager_v1 *vp =
		wlr_virtual_pointer_manager_v1_create(server->display);
	new_virtual_pointer_listener.notify = new_virtual_pointer;
	wl_signal_add(&vp->events.new_virtual_pointer, &new_virtual_pointer_listener);
}
