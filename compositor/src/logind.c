#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include "server.h"

/* Talk to systemd-logind so the screen is locked before the computer goes to
 * sleep (lid closed, suspend from the menu) and when "loginctl lock-session"
 * is used. */

static struct wl_event_source *release_timer;

static void take_sleep_lock(struct ts_server *server) {
	if (server->sleep_inhibit_fd >= 0 || server->bus == NULL) {
		return;
	}
	sd_bus *bus = server->bus;
	sd_bus_message *reply = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	int r = sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1",
		"org.freedesktop.login1.Manager", "Inhibit", &error, &reply, "ssss",
		"sleep", "Tessera", "Lock the screen before sleeping", "delay");
	if (r < 0) {
		wlr_log(WLR_INFO, "Could not delay sleep: %s", error.message);
	} else {
		int fd = -1;
		if (sd_bus_message_read(reply, "h", &fd) >= 0 && fd >= 0) {
			server->sleep_inhibit_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
		}
	}
	sd_bus_error_free(&error);
	sd_bus_message_unref(reply);
}

/* Let the desktop handle the power button (it opens the power options)
 * instead of the computer switching off at once. The lock is released
 * automatically when Tessera exits. */
static void take_power_key(struct ts_server *server) {
	sd_bus_message *reply = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	int r = sd_bus_call_method(server->bus, "org.freedesktop.login1", "/org/freedesktop/login1",
		"org.freedesktop.login1.Manager", "Inhibit", &error, &reply, "ssss",
		"handle-power-key", "Tessera", "Show the power options instead", "block");
	if (r >= 0) {
		int fd = -1;
		if (sd_bus_message_read(reply, "h", &fd) >= 0 && fd >= 0) {
			/* Deliberately kept open for the lifetime of the desktop. */
			fcntl(fd, F_DUPFD_CLOEXEC, 3);
		}
	}
	sd_bus_error_free(&error);
	sd_bus_message_unref(reply);
}

void logind_release_sleep_lock(struct ts_server *server) {
	if (server->sleep_inhibit_fd >= 0) {
		close(server->sleep_inhibit_fd);
		server->sleep_inhibit_fd = -1;
	}
}

static int release_fire(void *data) {
	logind_release_sleep_lock(data);
	return 0;
}

static int prepare_for_sleep(sd_bus_message *msg, void *data, sd_bus_error *error) {
	struct ts_server *server = data;
	int going_down = 0;
	if (sd_bus_message_read(msg, "b", &going_down) < 0) {
		return 0;
	}
	if (going_down) {
		lock_engage(server);
		/* Give the lock screen a moment to reach the display. */
		wl_event_source_timer_update(release_timer, 300);
	} else {
		take_sleep_lock(server);
		idle_reset(server);
	}
	return 0;
}

static int session_lock(sd_bus_message *msg, void *data, sd_bus_error *error) {
	lock_engage(data);
	return 0;
}

static int bus_readable(int fd, uint32_t mask, void *data) {
	struct ts_server *server = data;
	sd_bus *bus = server->bus;
	while (sd_bus_process(bus, NULL) > 0) {
	}
	return 0;
}

void logind_init(struct ts_server *server) {
	sd_bus *bus = NULL;
	if (sd_bus_open_system(&bus) < 0) {
		wlr_log(WLR_INFO, "No system bus; lock-before-sleep is disabled");
		return;
	}
	server->bus = bus;

	char session_path[256] = "/org/freedesktop/login1/session/auto";
	const char *id = getenv("XDG_SESSION_ID");
	sd_bus_message *reply = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	int r;
	if (id && id[0]) {
		r = sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1",
			"org.freedesktop.login1.Manager", "GetSession", &error, &reply, "s", id);
	} else {
		r = sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1",
			"org.freedesktop.login1.Manager", "GetSessionByPID", &error, &reply, "u",
			(uint32_t)getpid());
	}
	if (r >= 0) {
		const char *path = NULL;
		if (sd_bus_message_read(reply, "o", &path) >= 0 && path) {
			snprintf(session_path, sizeof(session_path), "%s", path);
		}
	}
	sd_bus_error_free(&error);
	sd_bus_message_unref(reply);

	sd_bus_match_signal(bus, NULL, "org.freedesktop.login1", "/org/freedesktop/login1",
		"org.freedesktop.login1.Manager", "PrepareForSleep", prepare_for_sleep, server);
	sd_bus_match_signal(bus, NULL, "org.freedesktop.login1", session_path,
		"org.freedesktop.login1.Session", "Lock", session_lock, server);

	release_timer = wl_event_loop_add_timer(server->loop, release_fire, server);
	take_sleep_lock(server);
	take_power_key(server);

	server->bus_source = wl_event_loop_add_fd(server->loop, sd_bus_get_fd(bus),
		WL_EVENT_READABLE, bus_readable, server);
	bus_readable(0, 0, server);
}
