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

static void lock_redraw(struct ts_server *server) {
	struct ts_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct ts_lock_surface *ls = output->lock_surface;
		if (ls == NULL) {
			continue;
		}
		struct wlr_box area = output->full_area;
		struct wlr_buffer *buf = deco_render_lock(server, area.width, area.height,
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
	server->auth_failed = false;
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
				free(replies);
				return PAM_BUF_ERR;
			}
		}
	}
	*resp = replies;
	return PAM_SUCCESS;
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
	if (n == 1 && result == 1) {
		lock_release(server);
	} else {
		server->auth_failed = true;
		lock_redraw(server);
	}
	return 0;
}

static void start_auth(struct ts_server *server) {
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) < 0) {
		return;
	}
	struct passwd *pw = getpwuid(getuid());
	const char *user = pw ? pw->pw_name : getenv("USER");

	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return;
	}
	if (pid == 0) {
		close(fds[0]);
		signal(SIGCHLD, SIG_DFL);
		struct pam_data data = { .password = server->password };
		struct pam_conv conv = { pam_conversation, &data };
		pam_handle_t *handle = NULL;
		char ok = 0;
		if (pam_start("tessera", user, &conv, &handle) == PAM_SUCCESS) {
			if (pam_authenticate(handle, 0) == PAM_SUCCESS) {
				ok = 1;
			}
			pam_end(handle, PAM_SUCCESS);
		}
		if (write(fds[1], &ok, 1) < 0) {
			_exit(1);
		}
		_exit(0);
	}
	close(fds[1]);
	explicit_bzero(server->password, sizeof(server->password));
	server->password_len = 0;
	server->auth_pid = pid;
	server->auth_fd = fds[0];
	server->auth_busy = true;
	server->auth_failed = false;
	server->auth_source = wl_event_loop_add_fd(server->loop, fds[0],
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

bool lock_handle_key(struct ts_server *server, xkb_keysym_t sym, uint32_t codepoint) {
	if (!server->locked || server->auth_busy) {
		return true;
	}
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
	server->clock_timer = wl_event_loop_add_timer(server->loop, clock_tick, server);
	wl_event_source_timer_update(server->clock_timer, 15000);
}
