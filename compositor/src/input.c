#define _POSIX_C_SOURCE 200809L
#include <libinput.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/backend/libinput.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
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
		input_constraint_update(server);
		return;
	}
	if (server->locked) {
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
	input_constraint_update(server);
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
		/* A keyboard made by an app keeps the layout that app gave it. */
		if (!kb->is_virtual) {
			wlr_keyboard_set_keymap(kb->wlr_keyboard, keymap);
		}
	}
	xkb_keymap_unref(keymap);
}

static void send_key_event(struct ts_server *server, const char *action) {
	char line[128];
	snprintf(line, sizeof(line), "{\"event\":\"key\",\"action\":\"%s\"}", action);
	ipc_broadcast(server, line);
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
			switcher_step(server, WLR_MODIFIER_ALT, shift ? -1 : 1);
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
	if (dx && ctrl) {
		workspace_step(server, dx);
		return true;
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
		switcher_step(server, WLR_MODIFIER_LOGO, shift ? -1 : 1);
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
		/* A virtual keyboard may type here on purpose. An on-screen keyboard
		 * or a switch device has to be able to reach the password field, and
		 * neither can invent a password you did not type - which is why the
		 * lock screen lets the protocol connect at all (see filter_global in
		 * main.c). Refusing its keys, as this did, left a machine without a
		 * keyboard with a screen nothing could be typed into.
		 *
		 * What a virtual keyboard does *not* get is the rest of the branch:
		 * switching virtual terminals and the media keys stay with a real
		 * keyboard. A program in the session has no business moving the
		 * person to another console, and it is not something a switch device
		 * or an on-screen keyboard ever needs.
		 *
		 * A wrong password from one of these costs the same 2-to-60-second
		 * wait as one from a person typing. */
		if (pressed) {
			bool done = false;
			if (!keyboard->is_virtual) {
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
	if (pressed && server->switcher_active) {
		xkb_keysym_t sym = xkb_state_key_get_one_sym(state, keycode);
		if (sym == XKB_KEY_Escape) {
			switcher_finish(server, false);
			return;
		}
		if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
			switcher_finish(server, true);
			return;
		}
		if (sym == XKB_KEY_Left || sym == XKB_KEY_Right) {
			switcher_step(server, server->switcher_mod, sym == XKB_KEY_Left ? -1 : 1);
			return;
		}
	}
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
	struct ts_server *server = keyboard->server;
	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(server->seat, &keyboard->wlr_keyboard->modifiers);
	switcher_modifiers(server, wlr_keyboard_get_modifiers(keyboard->wlr_keyboard));
	if (!keyboard->is_virtual) {
		lock_keyboard_changed(server); /* Caps Lock note on the lock screen */
	}
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
	/* A keyboard unplugged while choosing a window can't release Alt. */
	if (server->switcher_active) {
		switcher_finish(server, true);
	}
}

static void new_keyboard_ex(struct ts_server *server, struct wlr_input_device *device,
		bool is_virtual) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
	struct ts_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	if (keyboard == NULL) {
		wlr_log(WLR_ERROR, "Out of memory");
		return;
	}
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;
	keyboard->is_virtual = is_virtual;

	if (!is_virtual) {
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
	new_keyboard_ex(server, device, false);
}

static void update_capabilities(struct ts_server *server) {
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	struct ts_pointer *pointer;
	wl_list_for_each(pointer, &server->pointers, link) {
		if (pointer->device->type == WLR_INPUT_DEVICE_TOUCH) {
			caps |= WL_SEAT_CAPABILITY_TOUCH;
		}
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

/* On-screen keyboards and similar tools. */
static void new_virtual_keyboard(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *vkbd = data;
	new_keyboard_ex(server, &vkbd->keyboard.base, true);
	update_capabilities(server);
}

static void new_virtual_pointer(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, new_virtual_pointer);
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	wlr_cursor_attach_input_device(server->cursor, &event->new_pointer->pointer.base);
	update_capabilities(server);
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
	if (libinput_device_config_accel_is_available(li)) {
		libinput_device_config_accel_set_speed(li, server->config.pointer_speed);
	}
}

static void pointer_destroy(struct wl_listener *listener, void *data) {
	struct ts_pointer *pointer = wl_container_of(listener, pointer, destroy);
	struct ts_server *server = pointer->server;
	wl_list_remove(&pointer->destroy.link);
	wl_list_remove(&pointer->link);
	free(pointer);
	update_capabilities(server); /* the last touch screen may have gone */
}

/* True for a mouse or touchpad, false for a pointer made by an app. */
static bool pointer_is_real(struct ts_server *server, struct wlr_pointer *wlr_pointer) {
	struct ts_pointer *pointer;
	wl_list_for_each(pointer, &server->pointers, link) {
		if (pointer->device == &wlr_pointer->base) {
			return true;
		}
	}
	return false;
}

static void new_pointer(struct ts_server *server, struct wlr_input_device *device) {
	configure_libinput(server, device);
	wlr_cursor_attach_input_device(server->cursor, device);
	struct ts_pointer *pointer = calloc(1, sizeof(*pointer));
	if (pointer == NULL) {
		wlr_log(WLR_ERROR, "Out of memory");
		return;
	}
	pointer->server = server;
	pointer->device = device;
	pointer->destroy.notify = pointer_destroy;
	wl_signal_add(&device->events.destroy, &pointer->destroy);
	wl_list_insert(&server->pointers, &pointer->link);
	input_map_devices(server);
}

/* A touch screen gives positions on its own screen: the one it names, or
 * else the laptop's built-in screen (all screens if neither is in use).
 * Done again whenever the screens change, so it never points at a screen
 * that has gone. Drawing tablets cover all screens. */
void input_map_devices(struct ts_server *server) {
	struct ts_pointer *pointer;
	wl_list_for_each(pointer, &server->pointers, link) {
		struct wlr_input_device *device = pointer->device;
		if (device->type != WLR_INPUT_DEVICE_TOUCH) {
			continue;
		}
		const char *name = wlr_touch_from_input_device(device)->output_name;
		struct ts_output *target = name ? output_by_name(server, name) : output_builtin(server);
		wlr_cursor_map_input_to_output(server->cursor, device,
			target ? target->wlr_output : NULL);
	}
}

void pointers_apply_config(struct ts_server *server) {
	struct ts_pointer *pointer;
	wl_list_for_each(pointer, &server->pointers, link) {
		configure_libinput(server, pointer->device);
	}
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

static void update_pointer_focus(struct ts_server *server, uint32_t time) {
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
			if (server->focused_view == view) {
				server->active_output = output;
			}
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
		lock_handle_motion(server, lx, ly);
		set_cursor_image(server,
			server->lock_hover != TS_LOCK_BUTTON_NONE ? "pointer" : "default");
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

/* ---------------------------------------------------------------------- */
/* Pointer locking: games and 3D apps can hold the pointer in place (they   */
/* still get its movements) or keep it inside their window.                 */
/* ---------------------------------------------------------------------- */

struct ts_constraint {
	struct ts_server *server;
	struct wlr_pointer_constraint_v1 *constraint;
	struct wl_listener destroy;
};

/* A held pointer may say where the cursor should be when it is let go. */
static void warp_to_cursor_hint(struct ts_server *server,
		struct wlr_pointer_constraint_v1 *constraint) {
	struct wlr_seat *seat = server->seat;
	if (constraint->type != WLR_POINTER_CONSTRAINT_V1_LOCKED ||
			!constraint->current.cursor_hint.enabled ||
			seat->pointer_state.focused_surface != constraint->surface) {
		return;
	}
	double hx = constraint->current.cursor_hint.x, hy = constraint->current.cursor_hint.y;
	wlr_cursor_warp(server->cursor, NULL, server->cursor->x - seat->pointer_state.sx + hx,
		server->cursor->y - seat->pointer_state.sy + hy);
	wlr_seat_pointer_warp(seat, hx, hy);
}

/* Only the app that has the keyboard, while the pointer is over it (and in
 * the area it chose); never on the lock screen or while dragging a window. */
static struct wlr_pointer_constraint_v1 *wanted_constraint(struct ts_server *server) {
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = seat->keyboard_state.focused_surface;
	if (server->locked || server->cursor_mode != TS_CURSOR_PASSTHROUGH || surface == NULL ||
			seat->pointer_state.focused_surface != surface) {
		return NULL;
	}
	struct wlr_pointer_constraint_v1 *constraint = wlr_pointer_constraints_v1_constraint_for_surface(
		server->pointer_constraints, surface, seat);
	if (constraint == NULL || constraint == server->active_constraint) {
		return constraint;
	}
	if (!pixman_region32_contains_point(&constraint->region, (int)floor(seat->pointer_state.sx),
			(int)floor(seat->pointer_state.sy), NULL)) {
		return NULL;
	}
	return constraint;
}

void input_constraint_update(struct ts_server *server) {
	struct wlr_pointer_constraint_v1 *constraint = wanted_constraint(server);
	struct wlr_pointer_constraint_v1 *old = server->active_constraint;
	if (constraint == old) {
		return;
	}
	server->active_constraint = constraint;
	if (old) {
		warp_to_cursor_hint(server, old);
		wlr_pointer_constraint_v1_send_deactivated(old); /* may destroy it */
	}
	if (constraint) {
		wlr_pointer_constraint_v1_send_activated(constraint);
	}
}

/* Applies the active lock to a movement. False: the pointer must stay. */
static bool constrain_motion(struct ts_server *server, double *dx, double *dy) {
	input_constraint_update(server);
	struct wlr_pointer_constraint_v1 *constraint = server->active_constraint;
	struct wlr_seat *seat = server->seat;
	if (constraint == NULL || seat->pointer_state.focused_surface != constraint->surface) {
		return true;
	}
	if (constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
		return false;
	}
	double sx = seat->pointer_state.sx, sy = seat->pointer_state.sy;
	double nx, ny;
	if (!wlr_region_confine(&constraint->region, sx, sy, sx + *dx, sy + *dy, &nx, &ny)) {
		return false;
	}
	*dx = nx - sx;
	*dy = ny - sy;
	return true;
}

static void constraint_destroy(struct wl_listener *listener, void *data) {
	struct ts_constraint *c = wl_container_of(listener, c, destroy);
	struct ts_server *server = c->server;
	if (server->active_constraint == c->constraint) {
		warp_to_cursor_hint(server, c->constraint);
		server->active_constraint = NULL;
	}
	wl_list_remove(&c->destroy.link);
	free(c);
}

static void new_constraint(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, new_constraint);
	struct wlr_pointer_constraint_v1 *constraint = data;
	struct ts_constraint *c = calloc(1, sizeof(*c));
	if (c == NULL) {
		wlr_log(WLR_ERROR, "Out of memory");
		return;
	}
	c->server = server;
	c->constraint = constraint;
	c->destroy.notify = constraint_destroy;
	wl_signal_add(&constraint->events.destroy, &c->destroy);
	input_constraint_update(server);
}

static void process_motion(struct ts_server *server, uint32_t time) {
	update_pointer_focus(server, time);
	input_constraint_update(server);
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
	double dx = event->delta_x, dy = event->delta_y;
	if (!constrain_motion(server, &dx, &dy)) {
		return;
	}
	wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
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
	if (!constrain_motion(server, &dx, &dy)) {
		return;
	}
	if (server->active_constraint) {
		wlr_cursor_warp_closest(server->cursor, &event->pointer->base,
			server->cursor->x + dx, server->cursor->y + dy);
	} else {
		wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
	}
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
	struct ts_output *ratio_output = server->ratio_output;
	server->cursor_mode = TS_CURSOR_PASSTHROUGH;
	server->grab_view = NULL;
	server->ratio_output = NULL;
	server->cursor_image = NULL;
	if (ratio_output) {
		arrange_output(ratio_output); /* draws the corners hidden during the drag */
	}
	cursor_rebase(server);
}

/* Stops a window drag or resize without finishing it (the screen locked). */
void input_cancel_grab(struct ts_server *server) {
	if (server->cursor_mode == TS_CURSOR_PASSTHROUGH) {
		return;
	}
	update_drop_hint(server, NULL);
	struct ts_output *ratio_output = server->ratio_output;
	server->cursor_mode = TS_CURSOR_PASSTHROUGH;
	server->grab_view = NULL;
	server->ratio_output = NULL;
	server->cursor_image = NULL;
	if (ratio_output) {
		arrange_output(ratio_output); /* draws the corners hidden during the drag */
	}
}

void input_view_gone(struct ts_view *view) {
	struct ts_server *server = view->server;
	if (server->title_click_view == view) {
		server->title_click_view = NULL;
	}
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

/* A click gives the keyboard to the window or panel under it. */
static void focus_hit(struct ts_server *server, const struct hit *hit) {
	if (hit->view) {
		view_focus(hit->view);
	} else if (hit->layer && hit->layer->layer_surface->current.keyboard_interactive !=
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
		server->focused_layer = hit->layer;
		seat_focus_surface(server, hit->layer->layer_surface->surface);
	}
}

/* A mouse button, or what acts as one (a finger, a tablet pen). `real`: from
 * a device, not made by an app. */
static void pointer_button(struct ts_server *server, uint32_t time_msec, uint32_t button,
		enum wl_pointer_button_state state, bool real) {
	notify_activity(server);
	server->super_alone = false;

	bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
	if (server->locked) {
		/* Only a real device can use the lock screen's buttons. */
		if (pressed && button == BTN_LEFT && real) {
			lock_handle_click(server, server->cursor->x, server->cursor->y);
		}
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
			if (button == BTN_LEFT) {
				input_begin_move(hit.view);
			} else if (button == BTN_RIGHT && hit.view->floating) {
				input_begin_resize(hit.view, edges_for_point(hit.view, lx, ly));
			}
			return;
		}

		if (hit.view && hit.on_frame) {
			view_focus(hit.view);
			if (button != BTN_LEFT) {
				return;
			}
			if (hit.view->ssd && hit.fy < deco_titlebar_height() &&
					deco_close_hit(hit.view->box.width, hit.fx, hit.fy)) {
				server->cursor_mode = TS_CURSOR_CLOSE_PRESS;
				server->grab_view = hit.view;
				return;
			}
			/* A double click on the title bar maximizes. */
			if (hit.fy < deco_titlebar_height() && server->title_click_view == hit.view &&
					time_msec - server->title_click_time < 400) {
				server->title_click_view = NULL;
				view_set_maximized(hit.view, !hit.view->maximized);
				return;
			}
			server->title_click_time = time_msec;
			server->title_click_view = hit.view;
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

		if (!hit.view && !hit.layer && !hit.surface) {
			/* A click on a screen's desktop makes it the screen in use. */
			struct ts_output *output = output_at(server, lx, ly);
			if (output && output != output_focused(server)) {
				focus_output(server, output);
			}
		}

		if (!hit.view && !hit.layer && button == BTN_LEFT) {
			struct ts_output *output = output_at(server, lx, ly);
			if (layout_gap_hit(output, lx, ly)) {
				server->cursor_mode = TS_CURSOR_RATIO;
				server->ratio_output = output;
				return;
			}
		}

		focus_hit(server, &hit);
	}

	wlr_seat_pointer_notify_button(server->seat, time_msec, button, state);
}

static void cursor_button(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	pointer_button(server, event->time_msec, event->button, event->state,
		pointer_is_real(server, event->pointer));
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
/* Touch screens                                                            */
/* ---------------------------------------------------------------------- */

/* Whether the app has asked for touch input (most toolkits do). */
static bool surface_takes_touch(struct ts_server *server, struct wlr_surface *surface) {
	struct wlr_seat_client *seat_client = wlr_seat_client_for_wl_client(server->seat,
		wl_resource_get_client(surface->resource));
	return seat_client != NULL && !wl_list_empty(&seat_client->touches);
}

static int touch_point_index(struct ts_server *server, int32_t id) {
	for (int i = 0; i < server->touch_count; i++) {
		if (server->touch_points[i].id == id) {
			return i;
		}
	}
	return -1;
}

static void touch_point_remove(struct ts_server *server, int i) {
	server->touch_points[i] = server->touch_points[--server->touch_count];
}

/* Moves the pointer to the finger that acts as the mouse. */
static void touch_mouse_move(struct ts_server *server, struct wlr_touch *touch,
		double x, double y, uint32_t time_msec) {
	wlr_cursor_warp_absolute(server->cursor, &touch->base, x, y);
	process_motion(server, time_msec);
	wlr_seat_pointer_notify_frame(server->seat);
}

static void touch_mouse_button(struct ts_server *server, uint32_t time_msec,
		enum wl_pointer_button_state state) {
	pointer_button(server, time_msec, BTN_LEFT, state, true);
	wlr_seat_pointer_notify_frame(server->seat);
}

static void touch_down(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, touch_down);
	struct wlr_touch_down_event *event = data;
	notify_activity(server);
	if (server->cursor_mode != TS_CURSOR_PASSTHROUGH) {
		return; /* a window is being moved or resized */
	}
	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(server->cursor, &event->touch->base,
		event->x, event->y, &lx, &ly);
	if (!server->locked) {
		struct hit hit = hit_test(server, lx, ly);
		if (hit.surface && surface_takes_touch(server, hit.surface)) {
			if (server->touch_count == TS_TOUCH_POINTS) {
				return;
			}
			focus_hit(server, &hit);
			server->touch_points[server->touch_count++] = (struct ts_touch_point){
				event->touch_id, lx - hit.sx, ly - hit.sy };
			wlr_seat_touch_notify_down(server->seat, hit.surface, event->time_msec,
				event->touch_id, hit.sx, hit.sy);
			return;
		}
	}
	/* Anywhere else (title bars, the desktop, the lock screen, apps that
	 * don't take touch) one finger at a time acts as the mouse. */
	if (server->touch_as_mouse) {
		return;
	}
	server->touch_as_mouse = true;
	server->touch_mouse_id = event->touch_id;
	touch_mouse_move(server, event->touch, event->x, event->y, event->time_msec);
	touch_mouse_button(server, event->time_msec, WL_POINTER_BUTTON_STATE_PRESSED);
}

static void touch_motion(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, touch_motion);
	struct wlr_touch_motion_event *event = data;
	notify_activity(server);
	if (server->touch_as_mouse && event->touch_id == server->touch_mouse_id) {
		touch_mouse_move(server, event->touch, event->x, event->y, event->time_msec);
		return;
	}
	int i = touch_point_index(server, event->touch_id);
	if (i < 0 || server->locked) {
		return;
	}
	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(server->cursor, &event->touch->base,
		event->x, event->y, &lx, &ly);
	/* A file being dragged follows whatever started the drag, and a finger
	 * does not move the pointer: without this the icon sits wherever the
	 * mouse was left while the drop goes on under the finger. */
	if (server->seat->drag != NULL) {
		wlr_scene_node_set_position(&server->drag_icons->node, lx, ly);
	}
	wlr_seat_touch_notify_motion(server->seat, event->time_msec, event->touch_id,
		lx - server->touch_points[i].ox, ly - server->touch_points[i].oy);
}

static void touch_up(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, touch_up);
	struct wlr_touch_up_event *event = data;
	notify_activity(server);
	if (server->touch_as_mouse && event->touch_id == server->touch_mouse_id) {
		server->touch_as_mouse = false;
		touch_mouse_button(server, event->time_msec, WL_POINTER_BUTTON_STATE_RELEASED);
		return;
	}
	int i = touch_point_index(server, event->touch_id);
	if (i >= 0) {
		touch_point_remove(server, i);
		wlr_seat_touch_notify_up(server->seat, event->time_msec, event->touch_id);
	}
}

/* The touch screen gave up on a finger (for example, the screen turned off). */
static void touch_cancel(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, touch_cancel);
	struct wlr_touch_cancel_event *event = data;
	if (server->touch_as_mouse && event->touch_id == server->touch_mouse_id) {
		server->touch_as_mouse = false;
		touch_mouse_button(server, event->time_msec, WL_POINTER_BUTTON_STATE_RELEASED);
		return;
	}
	if (touch_point_index(server, event->touch_id) < 0) {
		return;
	}
	struct wlr_touch_point *point = wlr_seat_touch_get_point(server->seat, event->touch_id);
	if (point && point->client) {
		/* The app is told to forget all its fingers, and so are we. */
		wlr_seat_touch_notify_cancel(server->seat, point->client);
	}
	for (int i = server->touch_count - 1; i >= 0; i--) {
		if (wlr_seat_touch_get_point(server->seat, server->touch_points[i].id) == NULL) {
			touch_point_remove(server, i);
		}
	}
}

static void touch_frame(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, touch_frame);
	wlr_seat_touch_notify_frame(server->seat);
}

/* ---------------------------------------------------------------------- */
/* Drawing tablets: the pen works like a mouse (pressure and tilt are not  */
/* passed on).                                                              */
/* ---------------------------------------------------------------------- */

/* x or y is NAN when only the other one changed. */
static void tablet_move(struct ts_server *server, struct wlr_tablet *tablet,
		double x, double y, uint32_t time_msec) {
	notify_activity(server);
	wlr_cursor_warp_absolute(server->cursor, &tablet->base, x, y);
	process_motion(server, time_msec);
	wlr_seat_pointer_notify_frame(server->seat);
}

static void tablet_axis(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, tablet_axis);
	struct wlr_tablet_tool_axis_event *event = data;
	uint32_t axes = event->updated_axes;
	if (axes & (WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y)) {
		tablet_move(server, event->tablet, axes & WLR_TABLET_TOOL_AXIS_X ? event->x : NAN,
			axes & WLR_TABLET_TOOL_AXIS_Y ? event->y : NAN, event->time_msec);
	}
}

static void tablet_proximity(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, tablet_proximity);
	struct wlr_tablet_tool_proximity_event *event = data;
	if (event->state == WLR_TABLET_TOOL_PROXIMITY_IN) {
		tablet_move(server, event->tablet, event->x, event->y, event->time_msec);
	}
}

/* The pen's tip on the tablet is the left button. */
static void tablet_tip(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, tablet_tip);
	struct wlr_tablet_tool_tip_event *event = data;
	tablet_move(server, event->tablet, event->x, event->y, event->time_msec);
	pointer_button(server, event->time_msec, BTN_LEFT,
		event->state == WLR_TABLET_TOOL_TIP_DOWN ? WL_POINTER_BUTTON_STATE_PRESSED
			: WL_POINTER_BUTTON_STATE_RELEASED, true);
	wlr_seat_pointer_notify_frame(server->seat);
}

/* The pen's buttons: the lower one is the right button, the upper one the
 * middle button. */
static void tablet_button(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, tablet_button);
	struct wlr_tablet_tool_button_event *event = data;
	uint32_t button = event->button == BTN_STYLUS ? BTN_RIGHT
		: event->button == BTN_STYLUS2 ? BTN_MIDDLE : event->button;
	pointer_button(server, event->time_msec, button,
		event->state == WLR_BUTTON_PRESSED ? WL_POINTER_BUTTON_STATE_PRESSED
			: WL_POINTER_BUTTON_STATE_RELEASED, true);
	wlr_seat_pointer_notify_frame(server->seat);
}

/* ---------------------------------------------------------------------- */
/* Touchpad gestures                                                        */
/* ---------------------------------------------------------------------- */

#define SWIPE_DISTANCE 100.0

/* Three-finger swipes belong to the desktop; everything else (pinch to
 * zoom, other swipes) goes to the app under the cursor. */
static void swipe_begin(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, swipe_begin);
	struct wlr_pointer_swipe_begin_event *event = data;
	notify_activity(server);
	if (server->locked) {
		return;
	}
	if (server->config.gestures && event->fingers == 3) {
		server->swipe_active = true;
		server->swipe_dx = 0;
		server->swipe_dy = 0;
		return;
	}
	wlr_pointer_gestures_v1_send_swipe_begin(server->pointer_gestures, server->seat,
		event->time_msec, event->fingers);
}

static void swipe_update(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, swipe_update);
	struct wlr_pointer_swipe_update_event *event = data;
	notify_activity(server);
	if (server->swipe_active) {
		server->swipe_dx += event->dx;
		server->swipe_dy += event->dy;
		return;
	}
	if (!server->locked) {
		wlr_pointer_gestures_v1_send_swipe_update(server->pointer_gestures, server->seat,
			event->time_msec, event->dx, event->dy);
	}
}

static void swipe_end(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, swipe_end);
	struct wlr_pointer_swipe_end_event *event = data;
	if (!server->swipe_active) {
		if (!server->locked) {
			wlr_pointer_gestures_v1_send_swipe_end(server->pointer_gestures, server->seat,
				event->time_msec, event->cancelled);
		}
		return;
	}
	server->swipe_active = false;
	if (event->cancelled || server->locked) {
		return;
	}
	double dx = server->swipe_dx, dy = server->swipe_dy;
	double adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
	if (adx >= SWIPE_DISTANCE && adx > ady) {
		/* Fingers moving left bring in the workspace on the right. */
		int direction = dx < 0 ? 1 : -1;
		if (server->config.swipe_reversed) {
			direction = -direction;
		}
		workspace_step(server, direction);
	} else if (ady >= SWIPE_DISTANCE && ady > adx) {
		send_key_event(server, dy < 0 ? "launcher-show" : "launcher-hide");
	}
}

static void pinch_begin(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, pinch_begin);
	struct wlr_pointer_pinch_begin_event *event = data;
	notify_activity(server);
	if (!server->locked) {
		wlr_pointer_gestures_v1_send_pinch_begin(server->pointer_gestures, server->seat,
			event->time_msec, event->fingers);
	}
}

static void pinch_update(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, pinch_update);
	struct wlr_pointer_pinch_update_event *event = data;
	notify_activity(server);
	if (!server->locked) {
		wlr_pointer_gestures_v1_send_pinch_update(server->pointer_gestures, server->seat,
			event->time_msec, event->dx, event->dy, event->scale, event->rotation);
	}
}

static void pinch_end(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, pinch_end);
	struct wlr_pointer_pinch_end_event *event = data;
	if (!server->locked) {
		wlr_pointer_gestures_v1_send_pinch_end(server->pointer_gestures, server->seat,
			event->time_msec, event->cancelled);
	}
}

static void hold_begin(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, hold_begin);
	struct wlr_pointer_hold_begin_event *event = data;
	notify_activity(server);
	if (!server->locked) {
		wlr_pointer_gestures_v1_send_hold_begin(server->pointer_gestures, server->seat,
			event->time_msec, event->fingers);
	}
}

static void hold_end(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, hold_end);
	struct wlr_pointer_hold_end_event *event = data;
	if (!server->locked) {
		wlr_pointer_gestures_v1_send_hold_end(server->pointer_gestures, server->seat,
			event->time_msec, event->cancelled);
	}
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
		return;
	}
	/* A drag can start from a finger as well as from a pointer, and the serial
	 * it carries is then the touch's. Only the pointer was asked, so every
	 * touchscreen drag ended in the branch below: the data source was
	 * destroyed and the program that offered the file was told the drag had
	 * been cancelled. The rest of this desktop takes touch seriously - tap to
	 * click, two-finger scrolling, the on-screen keyboard on the lock screen -
	 * so this was a hole, not a decision. The point comes back with the answer
	 * and is what the drag then follows. */
	struct wlr_touch_point *point = NULL;
	if (wlr_seat_validate_touch_grab_serial(server->seat, event->origin, event->serial,
				&point) && point != NULL) {
		wlr_seat_start_touch_drag(server->seat, event->drag, event->serial, point);
		return;
	}
	wlr_data_source_destroy(event->drag->source);
}

/* The icon that follows the pointer while a file is dragged between two
 * programs. The tree belongs to wlroots, not to us: wlr_scene_drag_icon_create()
 * adds its own listener to the icon's destroy signal and takes the tree down
 * there, and it frees the wrapper it allocated when that tree goes.
 *
 * This used to keep the tree as well and destroy it a second time when the drag
 * ended - a node wlroots had already freed. Dragging a file from one program to
 * another therefore killed the session and put the login screen back, every
 * time. Nothing here may destroy that tree. */
static void start_drag(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, start_drag);
	struct wlr_drag *drag = data;
	if (drag->icon == NULL) {
		return;
	}
	if (wlr_scene_drag_icon_create(server->drag_icons, drag->icon) == NULL) {
		wlr_log(WLR_ERROR, "Out of memory");
	}
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
	server->swipe_begin.notify = swipe_begin;
	wl_signal_add(&server->cursor->events.swipe_begin, &server->swipe_begin);
	server->swipe_update.notify = swipe_update;
	wl_signal_add(&server->cursor->events.swipe_update, &server->swipe_update);
	server->swipe_end.notify = swipe_end;
	wl_signal_add(&server->cursor->events.swipe_end, &server->swipe_end);
	server->pinch_begin.notify = pinch_begin;
	wl_signal_add(&server->cursor->events.pinch_begin, &server->pinch_begin);
	server->pinch_update.notify = pinch_update;
	wl_signal_add(&server->cursor->events.pinch_update, &server->pinch_update);
	server->pinch_end.notify = pinch_end;
	wl_signal_add(&server->cursor->events.pinch_end, &server->pinch_end);
	server->hold_begin.notify = hold_begin;
	wl_signal_add(&server->cursor->events.hold_begin, &server->hold_begin);
	server->hold_end.notify = hold_end;
	wl_signal_add(&server->cursor->events.hold_end, &server->hold_end);
	server->touch_down.notify = touch_down;
	wl_signal_add(&server->cursor->events.touch_down, &server->touch_down);
	server->touch_motion.notify = touch_motion;
	wl_signal_add(&server->cursor->events.touch_motion, &server->touch_motion);
	server->touch_up.notify = touch_up;
	wl_signal_add(&server->cursor->events.touch_up, &server->touch_up);
	server->touch_cancel.notify = touch_cancel;
	wl_signal_add(&server->cursor->events.touch_cancel, &server->touch_cancel);
	server->touch_frame.notify = touch_frame;
	wl_signal_add(&server->cursor->events.touch_frame, &server->touch_frame);
	server->tablet_axis.notify = tablet_axis;
	wl_signal_add(&server->cursor->events.tablet_tool_axis, &server->tablet_axis);
	server->tablet_proximity.notify = tablet_proximity;
	wl_signal_add(&server->cursor->events.tablet_tool_proximity, &server->tablet_proximity);
	server->tablet_tip.notify = tablet_tip;
	wl_signal_add(&server->cursor->events.tablet_tool_tip, &server->tablet_tip);
	server->tablet_button.notify = tablet_button;
	wl_signal_add(&server->cursor->events.tablet_tool_button, &server->tablet_button);

	wl_list_init(&server->keyboards);
	wl_list_init(&server->pointers);
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
	server->pointer_constraints = wlr_pointer_constraints_v1_create(server->display);
	server->new_constraint.notify = new_constraint;
	wl_signal_add(&server->pointer_constraints->events.new_constraint, &server->new_constraint);
	server->cursor_shape = wlr_cursor_shape_manager_v1_create(server->display, 1);
	server->request_set_shape.notify = request_set_shape;
	wl_signal_add(&server->cursor_shape->events.request_set_shape, &server->request_set_shape);

	float none[4] = {0, 0, 0, 0};
	server->drop_hint = wlr_scene_rect_create(server->layer_drag, 1, 1, none);
	wlr_scene_node_set_enabled(&server->drop_hint->node, false);
	server->drag_icons = wlr_scene_tree_create(server->layer_drag);

	server->virtual_keyboard = wlr_virtual_keyboard_manager_v1_create(server->display);
	server->new_virtual_keyboard.notify = new_virtual_keyboard;
	wl_signal_add(&server->virtual_keyboard->events.new_virtual_keyboard,
		&server->new_virtual_keyboard);
	server->virtual_pointer = wlr_virtual_pointer_manager_v1_create(server->display);
	server->new_virtual_pointer.notify = new_virtual_pointer;
	wl_signal_add(&server->virtual_pointer->events.new_virtual_pointer,
		&server->new_virtual_pointer);
}
