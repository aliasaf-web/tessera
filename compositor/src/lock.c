#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include "server.h"

/* The lock screen is drawn by the compositor itself, so nothing can show
 * through it and it cannot crash separately from the desktop. Passwords
 * are checked with PAM in a short-lived child process. */

static int current_layout(struct ts_server *server);

static void lock_redraw(struct ts_server *server) {
	struct ts_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct ts_lock_surface *ls = output->lock_surface;
		if (ls == NULL) {
			continue;
		}
		struct wlr_box area = output->full_area;
		struct wlr_buffer *buf = deco_render_lock(server, output, area.width, area.height,
			output->wlr_output->scale);
		if (buf) {
			wlr_scene_buffer_set_buffer(ls->buffer, buf);
			wlr_scene_buffer_set_dest_size(ls->buffer, area.width, area.height);
			wlr_buffer_drop(buf);
		}
		wlr_scene_node_set_position(&ls->tree->node, area.x, area.y);
	}
}

void lock_arrange(struct ts_server *server) {
	if (!server->locked) {
		return;
	}
	struct ts_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->lock_surface == NULL) {
			struct ts_lock_surface *ls = calloc(1, sizeof(*ls));
			if (ls == NULL) {
				wlr_log(WLR_ERROR, "Out of memory");
				continue;
			}
			ls->node.kind = TS_NODE_LOCK;
			ls->output = output;
			ls->tree = wlr_scene_tree_create(server->layer_lock);
			ls->tree->node.data = &ls->node;
			ls->buffer = wlr_scene_buffer_create(ls->tree, NULL);
			output->lock_surface = ls;
		}
	}
	lock_redraw(server);
}

void lock_output_destroyed(struct ts_output *output) {
	if (output->lock_surface) {
		wlr_scene_node_destroy(&output->lock_surface->tree->node);
		free(output->lock_surface);
		output->lock_surface = NULL;
	}
}

static int clock_tick(void *data) {
	struct ts_server *server = data;
	if (server->locked) {
		lock_redraw(server);
	}
	wl_event_source_timer_update(server->clock_timer, 15000);
	return 0;
}

void lock_engage(struct ts_server *server) {
	if (server->locked) {
		return;
	}
	server->locked = true;
	server->lock_hover = TS_LOCK_BUTTON_NONE;
	server->lock_armed = TS_LOCK_BUTTON_NONE;
	server->lock_caps = lock_caps_lock_on(server);
	server->lock_layout = current_layout(server);
	input_cancel_grab(server);
	if (server->switcher_active) {
		switcher_finish(server, false);
	}
	server->password_len = 0;
	explicit_bzero(server->password, sizeof(server->password));
	server->auth_failed = false;
	wlr_seat_keyboard_notify_clear_focus(server->seat);
	wlr_seat_pointer_clear_focus(server->seat);
	lock_arrange(server);
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	server->cursor_image = "default";
	ipc_send_state(server);
}

static void lock_release(struct ts_server *server) {
	server->locked = false;
	server->lock_armed = TS_LOCK_BUTTON_NONE;
	wl_event_source_timer_update(server->lock_arm_timer, 0);
	server->auth_failed = false;
	server->auth_failures = 0;
	server->auth_wait = 0;
	wl_event_source_timer_update(server->auth_wait_timer, 0);
	server->password_len = 0;
	explicit_bzero(server->password, sizeof(server->password));
	struct ts_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		lock_output_destroyed(output);
	}
	struct ts_layer_surface *exclusive = layer_wants_exclusive_focus(server);
	if (exclusive) {
		server->focused_layer = exclusive;
		seat_focus_surface(server, exclusive->layer_surface->surface);
	} else if (server->focused_view) {
		struct ts_view *view = server->focused_view;
		server->focused_view = NULL;
		view_focus(view);
	} else {
		focus_top_view(server);
	}
	server->cursor_image = NULL;
	cursor_rebase(server);
	idle_reset(server);
	ipc_send_state(server);
}

struct pam_data {
	const char *password;
};

static int pam_conversation(int num, const struct pam_message **msg,
		struct pam_response **resp, void *appdata) {
	struct pam_data *data = appdata;
	struct pam_response *replies = calloc(num, sizeof(*replies));
	if (replies == NULL) {
		return PAM_BUF_ERR;
	}
	for (int i = 0; i < num; i++) {
		if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
				msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
			replies[i].resp = strdup(data->password);
			if (replies[i].resp == NULL) {
				for (int j = 0; j < i; j++) {
					free(replies[j].resp);
				}
				free(replies);
				return PAM_BUF_ERR;
			}
		}
	}
	*resp = replies;
	return PAM_SUCCESS;
}

/* `tessera --auth-helper`: reads a password on stdin, checks it with PAM
 * and writes "1" (correct) or "0" to stdout. Running this as a separate
 * program keeps PAM out of the desktop's own process. */
int lock_auth_helper_main(void) {
	char password[512];
	size_t len = 0;
	ssize_t n;
	while (len < sizeof(password) - 1 &&
			(n = read(STDIN_FILENO, password + len, sizeof(password) - 1 - len)) > 0) {
		len += n;
	}
	password[len] = '\0';

	struct passwd *pw = getpwuid(getuid());
	char ok = '0';
	if (pw != NULL) {
		struct pam_data data = { .password = password };
		struct pam_conv conv = { pam_conversation, &data };
		pam_handle_t *handle = NULL;
		if (pam_start("tessera", pw->pw_name, &conv, &handle) == PAM_SUCCESS) {
			if (pam_authenticate(handle, 0) == PAM_SUCCESS) {
				ok = '1';
			}
			pam_end(handle, PAM_SUCCESS);
		}
	}
	explicit_bzero(password, sizeof(password));
	if (write(STDOUT_FILENO, &ok, 1) != 1) {
		return 1;
	}
	return ok == '1' ? 0 : 1;
}

/* After a few wrong passwords in a row, each new try has to wait a little
 * longer: 2, 4, 8, 16 and then 30 seconds. */
static int wait_after_failures(int failures) {
	if (failures < 3) {
		return 0;
	}
	int doublings = failures - 2;
	return doublings >= 5 ? 30 : 1 << doublings;
}

static int wait_tick(void *data) {
	struct ts_server *server = data;
	if (server->auth_wait > 0) {
		server->auth_wait--;
	}
	if (server->auth_wait > 0) {
		wl_event_source_timer_update(server->auth_wait_timer, 1000);
	}
	lock_redraw(server);
	return 0;
}

static int auth_done(int fd, uint32_t mask, void *data) {
	struct ts_server *server = data;
	char result = 0;
	ssize_t n = read(fd, &result, 1);
	wl_event_source_remove(server->auth_source);
	server->auth_source = NULL;
	close(fd);
	server->auth_fd = -1;
	if (server->auth_pid > 0) {
		waitpid(server->auth_pid, NULL, 0);
		server->auth_pid = 0;
	}
	server->auth_busy = false;
	if (n == 1 && result == '1') {
		lock_release(server);
	} else {
		server->auth_failed = true;
		server->auth_failures++;
		server->auth_wait = wait_after_failures(server->auth_failures);
		if (server->auth_wait > 0) {
			wl_event_source_timer_update(server->auth_wait_timer, 1000);
		}
		lock_redraw(server);
	}
	return 0;
}

static void start_auth(struct ts_server *server) {
	int in[2], out[2];
	if (pipe2(in, O_CLOEXEC) < 0) {
		return;
	}
	if (pipe2(out, O_CLOEXEC) < 0) {
		close(in[0]);
		close(in[1]);
		return;
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(in[0]);
		close(in[1]);
		close(out[0]);
		close(out[1]);
		return;
	}
	if (pid == 0) {
		/* Only async-signal-safe calls between fork and exec. */
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		dup2(in[0], STDIN_FILENO);
		dup2(out[1], STDOUT_FILENO);
		execl("/proc/self/exe", "tessera", "--auth-helper", (char *)NULL);
		_exit(127);
	}
	close(in[0]);
	close(out[1]);
	size_t written = 0;
	while (written < server->password_len) {
		ssize_t n = write(in[1], server->password + written, server->password_len - written);
		if (n <= 0) {
			break;
		}
		written += n;
	}
	close(in[1]);
	explicit_bzero(server->password, sizeof(server->password));
	server->password_len = 0;
	server->auth_pid = pid;
	server->auth_fd = out[0];
	server->auth_busy = true;
	server->auth_failed = false;
	server->auth_source = wl_event_loop_add_fd(server->loop, out[0],
		WL_EVENT_READABLE | WL_EVENT_HANGUP, auth_done, server);
	lock_redraw(server);
}

static void remove_last_utf8(struct ts_server *server) {
	while (server->password_len > 0) {
		unsigned char c = server->password[--server->password_len];
		server->password[server->password_len] = '\0';
		if ((c & 0xC0) != 0x80) {
			break;
		}
	}
}

static void append_utf8(struct ts_server *server, uint32_t cp) {
	char out[4];
	size_t n;
	if (cp < 0x80) {
		out[0] = cp;
		n = 1;
	} else if (cp < 0x800) {
		out[0] = 0xC0 | (cp >> 6);
		out[1] = 0x80 | (cp & 0x3F);
		n = 2;
	} else if (cp < 0x10000) {
		out[0] = 0xE0 | (cp >> 12);
		out[1] = 0x80 | ((cp >> 6) & 0x3F);
		out[2] = 0x80 | (cp & 0x3F);
		n = 3;
	} else {
		out[0] = 0xF0 | (cp >> 18);
		out[1] = 0x80 | ((cp >> 12) & 0x3F);
		out[2] = 0x80 | ((cp >> 6) & 0x3F);
		out[3] = 0x80 | (cp & 0x3F);
		n = 4;
	}
	if (server->password_len + n >= sizeof(server->password)) {
		return;
	}
	memcpy(server->password + server->password_len, out, n);
	server->password_len += n;
	server->password[server->password_len] = '\0';
}

/* ---- Sleep, Restart and Shut down ---- */

static void disarm(struct ts_server *server) {
	if (server->lock_armed != TS_LOCK_BUTTON_NONE) {
		server->lock_armed = TS_LOCK_BUTTON_NONE;
		wl_event_source_timer_update(server->lock_arm_timer, 0);
		lock_redraw(server);
	}
}

static int arm_expired(void *data) {
	disarm(data);
	return 0;
}

/* Sleep happens at once. Restart and Shut down close every app, so they
 * ask for a second click within a few seconds. */
static void press_button(struct ts_server *server, enum ts_lock_button button) {
	if (button == TS_LOCK_BUTTON_SLEEP) {
		disarm(server);
		logind_power_action(server, "Suspend");
		return;
	}
	if (server->lock_armed != button) {
		server->lock_armed = button;
		wl_event_source_timer_update(server->lock_arm_timer, 5000);
		lock_redraw(server);
		return;
	}
	disarm(server);
	logind_power_action(server, button == TS_LOCK_BUTTON_RESTART ? "Reboot" : "PowerOff");
}

static enum ts_lock_button button_at(struct ts_server *server, double lx, double ly) {
	struct wlr_output *wlr_output = wlr_output_layout_output_at(server->output_layout, lx, ly);
	struct ts_output *output = wlr_output ? wlr_output->data : NULL;
	if (output == NULL) {
		return TS_LOCK_BUTTON_NONE;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, wlr_output, &box);
	for (int i = TS_LOCK_BUTTON_NONE + 1; i < TS_LOCK_BUTTON_COUNT; i++) {
		if (wlr_box_contains_point(&output->lock_buttons[i], lx - box.x, ly - box.y)) {
			return i;
		}
	}
	return TS_LOCK_BUTTON_NONE;
}

void lock_handle_motion(struct ts_server *server, double lx, double ly) {
	enum ts_lock_button hover = button_at(server, lx, ly);
	if (hover != server->lock_hover) {
		server->lock_hover = hover;
		lock_redraw(server);
	}
}

void lock_handle_click(struct ts_server *server, double lx, double ly) {
	enum ts_lock_button button = button_at(server, lx, ly);
	if (button == TS_LOCK_BUTTON_NONE) {
		disarm(server);
	} else {
		press_button(server, button);
	}
}

/* ---- Caps Lock and the keyboard layout, shown under the password ---- */

static struct ts_keyboard *real_keyboard(struct ts_server *server) {
	struct ts_keyboard *keyboard;
	wl_list_for_each(keyboard, &server->keyboards, link) {
		if (!keyboard->is_virtual && keyboard->wlr_keyboard->xkb_state) {
			return keyboard;
		}
	}
	return NULL;
}

bool lock_caps_lock_on(struct ts_server *server) {
	struct ts_keyboard *keyboard = real_keyboard(server);
	/* Asked of xkb: wlroots' modifier list leaves out locked keys. */
	return keyboard && xkb_state_mod_name_is_active(keyboard->wlr_keyboard->xkb_state,
		XKB_MOD_NAME_CAPS, XKB_STATE_MODS_EFFECTIVE) > 0;
}

static int current_layout(struct ts_server *server) {
	struct ts_keyboard *keyboard = real_keyboard(server);
	if (keyboard == NULL) {
		return -1;
	}
	return (int)xkb_state_serialize_layout(keyboard->wlr_keyboard->xkb_state,
		XKB_STATE_LAYOUT_EFFECTIVE);
}

/* The name of the layout in use, or NULL when there is only one. */
const char *lock_layout_name(struct ts_server *server) {
	struct ts_keyboard *keyboard = real_keyboard(server);
	if (keyboard == NULL || keyboard->wlr_keyboard->keymap == NULL ||
			xkb_keymap_num_layouts(keyboard->wlr_keyboard->keymap) < 2) {
		return NULL;
	}
	return xkb_keymap_layout_get_name(keyboard->wlr_keyboard->keymap,
		(xkb_layout_index_t)current_layout(server));
}

void lock_keyboard_changed(struct ts_server *server) {
	if (!server->locked) {
		return;
	}
	bool caps = lock_caps_lock_on(server);
	int layout = current_layout(server);
	if (caps != server->lock_caps || layout != server->lock_layout) {
		server->lock_caps = caps;
		server->lock_layout = layout;
		lock_redraw(server);
	}
}

bool lock_handle_key(struct ts_server *server, xkb_keysym_t sym, uint32_t codepoint) {
	if (sym == XKB_KEY_XF86PowerOff) {
		/* The power button puts the computer to sleep from here. */
		press_button(server, TS_LOCK_BUTTON_SLEEP);
		return true;
	}
	if (!server->locked || server->auth_busy || server->auth_wait > 0) {
		return true;
	}
	disarm(server);
	switch (sym) {
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		if (server->password_len > 0) {
			start_auth(server);
		}
		return true;
	case XKB_KEY_BackSpace:
		remove_last_utf8(server);
		break;
	case XKB_KEY_Escape:
		server->password_len = 0;
		explicit_bzero(server->password, sizeof(server->password));
		break;
	default:
		if (codepoint >= 0x20 && codepoint != 0x7F) {
			append_utf8(server, codepoint);
			server->auth_failed = false;
		} else {
			return true;
		}
	}
	lock_redraw(server);
	return true;
}

void lock_init(struct ts_server *server) {
	server->auth_wait_timer = wl_event_loop_add_timer(server->loop, wait_tick, server);
	server->lock_arm_timer = wl_event_loop_add_timer(server->loop, arm_expired, server);
	server->clock_timer = wl_event_loop_add_timer(server->loop, clock_tick, server);
	wl_event_source_timer_update(server->clock_timer, 15000);
}
