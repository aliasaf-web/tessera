#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include "server.h"

/* A tiny line-based protocol on $XDG_RUNTIME_DIR/tessera-$WAYLAND_DISPLAY.sock
 * Requests are plain words ("workspace 2", "lock"); replies and events are
 * one JSON object per line. */

static void socket_path(char *buf, size_t size, const char *display) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	snprintf(buf, size, "%s/tessera-%s.sock", runtime ? runtime : "/tmp",
		display ? display : "wayland-0");
}

static void json_escape(char *out, size_t size, const char *in) {
	size_t o = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p && o + 7 < size; p++) {
		if (*p == '"' || *p == '\\') {
			out[o++] = '\\';
			out[o++] = *p;
		} else if (*p < 0x20) {
			o += snprintf(out + o, size - o, "\\u%04x", *p);
		} else {
			out[o++] = *p;
		}
	}
	out[o] = '\0';
}

static void client_write(struct ts_ipc_client *client, const char *line) {
	size_t len = strlen(line);
	/* Non-blocking: a client that doesn't read just misses updates. */
	if (write(client->fd, line, len) < 0 || write(client->fd, "\n", 1) < 0) {
		return;
	}
}

static void client_destroy(struct ts_ipc_client *client) {
	wl_event_source_remove(client->source);
	close(client->fd);
	wl_list_remove(&client->link);
	free(client);
}

void ipc_broadcast(struct ts_server *server, const char *json_line) {
	struct ts_ipc_client *client, *tmp;
	wl_list_for_each_safe(client, tmp, &server->ipc_clients, link) {
		if (client->subscribed) {
			client_write(client, json_line);
		}
	}
}

static void state_json(struct ts_server *server, char *buf, size_t size) {
	struct ts_output *output = output_focused(server);
	int current = output ? output->workspace : 1;
	bool occupied[TS_WORKSPACES + 1] = {0};
	struct ts_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->mapped && view->output == output) {
			occupied[view->workspace] = true;
		}
	}
	char occ[64] = "";
	size_t o = 0;
	for (int i = 1; i <= TS_WORKSPACES; i++) {
		if (occupied[i]) {
			o += snprintf(occ + o, sizeof(occ) - o, "%s%d", o ? "," : "", i);
		}
	}
	char title[512] = "", app[256] = "";
	if (server->focused_view) {
		json_escape(title, sizeof(title), view_title(server->focused_view));
		json_escape(app, sizeof(app), view_app_id(server->focused_view));
	}
	snprintf(buf, size,
		"{\"event\":\"state\",\"workspace\":%d,\"workspaces\":%d,\"occupied\":[%s],"
		"\"title\":\"%s\",\"app_id\":\"%s\",\"locked\":%s,\"theme\":\"%s\"}",
		current, TS_WORKSPACES, occ, title, app, server->locked ? "true" : "false",
		server->theme.dark ? "dark" : "light");
}

void ipc_send_state(struct ts_server *server) {
	if (wl_list_empty(&server->ipc_clients)) {
		return;
	}
	char buf[1024];
	state_json(server, buf, sizeof(buf));
	ipc_broadcast(server, buf);
}

void config_reload(struct ts_server *server) {
	config_load(server);
	theme_from_config(server);
	keyboard_apply_config(server);
	struct ts_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		float bg[4] = { server->theme.background.r, server->theme.background.g,
			server->theme.background.b, 1.0f };
		if (output->background) {
			wlr_scene_rect_set_color(output->background, bg);
		}
	}
	views_redraw_frames(server);
	arrange_all(server);
	idle_reset(server);
	ipc_send_state(server);
}

static void handle_command(struct ts_ipc_client *client, char *line) {
	struct ts_server *server = client->server;
	char *arg = strchr(line, ' ');
	if (arg) {
		*arg++ = '\0';
		while (*arg == ' ') {
			arg++;
		}
	}
	const char *ok = "{\"ok\":true}";
	struct ts_view *focused = server->focused_view;

	if (strcmp(line, "subscribe") == 0) {
		client->subscribed = true;
		char buf[1024];
		state_json(server, buf, sizeof(buf));
		client_write(client, buf);
		return;
	} else if (strcmp(line, "state") == 0) {
		char buf[1024];
		state_json(server, buf, sizeof(buf));
		client_write(client, buf);
		return;
	} else if (strcmp(line, "workspace") == 0 && arg) {
		workspace_switch(server, atoi(arg));
	} else if (strcmp(line, "move-to-workspace") == 0 && arg) {
		if (focused) {
			view_move_to_workspace(focused, atoi(arg));
		}
	} else if (strcmp(line, "exec") == 0 && arg) {
		server_spawn(arg);
	} else if (strcmp(line, "close") == 0) {
		view_close(focused);
	} else if (strcmp(line, "fullscreen") == 0) {
		if (focused) {
			view_set_fullscreen(focused, !focused->fullscreen);
		}
	} else if (strcmp(line, "float") == 0) {
		if (focused) {
			view_set_floating(focused, !focused->floating);
		}
	} else if (strcmp(line, "lock") == 0) {
		lock_engage(server);
	} else if (strcmp(line, "reload") == 0) {
		config_reload(server);
	} else if (strcmp(line, "exit") == 0 || strcmp(line, "logout") == 0) {
		client_write(client, ok);
		wl_display_terminate(server->display);
		return;
	} else if (strcmp(line, "dpms") == 0 && arg) {
		output_set_dpms(server, strcmp(arg, "off") != 0);
	} else if (strcmp(line, "version") == 0) {
		client_write(client, "{\"version\":\"1.0.0\"}");
		return;
	} else {
		client_write(client, "{\"ok\":false,\"error\":\"unknown command\"}");
		return;
	}
	client_write(client, ok);
}

static int client_readable(int fd, uint32_t mask, void *data) {
	struct ts_ipc_client *client = data;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		client_destroy(client);
		return 0;
	}
	ssize_t n = read(fd, client->buf + client->len, sizeof(client->buf) - client->len - 1);
	if (n <= 0) {
		if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
			return 0;
		}
		client_destroy(client);
		return 0;
	}
	client->len += n;
	client->buf[client->len] = '\0';
	char *start = client->buf;
	char *nl;
	while ((nl = strchr(start, '\n')) != NULL) {
		*nl = '\0';
		if (nl > start && nl[-1] == '\r') {
			nl[-1] = '\0';
		}
		if (*start) {
			handle_command(client, start);
		}
		start = nl + 1;
	}
	size_t rest = client->len - (start - client->buf);
	memmove(client->buf, start, rest);
	client->len = rest;
	if (client->len >= sizeof(client->buf) - 1) {
		client->len = 0; /* line too long: drop it */
	}
	return 0;
}

static int server_readable(int fd, uint32_t mask, void *data) {
	struct ts_server *server = data;
	int cfd = accept(fd, NULL, NULL);
	if (cfd < 0) {
		return 0;
	}
	fcntl(cfd, F_SETFD, FD_CLOEXEC);
	fcntl(cfd, F_SETFL, O_NONBLOCK);
	struct ts_ipc_client *client = calloc(1, sizeof(*client));
	client->server = server;
	client->fd = cfd;
	client->source = wl_event_loop_add_fd(server->loop, cfd, WL_EVENT_READABLE,
		client_readable, client);
	wl_list_insert(&server->ipc_clients, &client->link);
	return 0;
}

void ipc_init(struct ts_server *server) {
	wl_list_init(&server->ipc_clients);
	socket_path(server->ipc_path, sizeof(server->ipc_path), server->socket_name);
	unlink(server->ipc_path);

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		wlr_log_errno(WLR_ERROR, "Could not create the control socket");
		return;
	}
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", server->ipc_path);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 8) < 0) {
		wlr_log_errno(WLR_ERROR, "Could not listen on %s", server->ipc_path);
		close(fd);
		return;
	}
	server->ipc_fd = fd;
	server->ipc_source = wl_event_loop_add_fd(server->loop, fd, WL_EVENT_READABLE,
		server_readable, server);
	setenv("TESSERA_SOCK", server->ipc_path, true);
}

void ipc_finish(struct ts_server *server) {
	struct ts_ipc_client *client, *tmp;
	wl_list_for_each_safe(client, tmp, &server->ipc_clients, link) {
		client_destroy(client);
	}
	if (server->ipc_source) {
		wl_event_source_remove(server->ipc_source);
	}
	if (server->ipc_fd >= 0) {
		close(server->ipc_fd);
		unlink(server->ipc_path);
	}
}

/* `tessera msg ...` */
int ipc_client_main(int argc, char **argv) {
	if (argc < 1) {
		fprintf(stderr, "Usage: tessera msg COMMAND [ARGUMENT]\n"
			"Commands: workspace N, move-to-workspace N, exec CMD, close, fullscreen,\n"
			"          float, lock, reload, logout, state, subscribe, dpms on|off\n");
		return 1;
	}
	char path[108];
	const char *env = getenv("TESSERA_SOCK");
	if (env && env[0]) {
		snprintf(path, sizeof(path), "%s", env);
	} else {
		socket_path(path, sizeof(path), getenv("WAYLAND_DISPLAY"));
	}
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "Tessera does not seem to be running (%s)\n", path);
		return 1;
	}
	char line[2048] = "";
	size_t o = 0;
	for (int i = 0; i < argc; i++) {
		o += snprintf(line + o, sizeof(line) - o, "%s%s", i ? " " : "", argv[i]);
	}
	o += snprintf(line + o, sizeof(line) - o, "\n");
	if (write(fd, line, o) < 0) {
		close(fd);
		return 1;
	}
	bool follow = strcmp(argv[0], "subscribe") == 0;
	char buf[4096];
	ssize_t n;
	int status = 0;
	while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
		buf[n] = '\0';
		fputs(buf, stdout);
		fflush(stdout);
		if (strstr(buf, "\"ok\":false")) {
			status = 1;
		}
		if (!follow && strchr(buf, '\n')) {
			break;
		}
	}
	close(fd);
	return status;
}
