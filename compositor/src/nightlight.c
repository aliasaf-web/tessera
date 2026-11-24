#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/util/log.h>
#include "server.h"

/* Night light: warmer colours in the evening by changing each screen's
 * colour curves (gamma). Turning it on or off from quick settings lasts
 * until the schedule next changes, like on phones. */

static void temperature_to_rgb(int kelvin, double *r, double *g, double *b) {
	double t = kelvin / 100.0;
	double red, green, blue;
	if (t <= 66) {
		red = 255;
		green = 99.4708025861 * log(t) - 161.1195681661;
	} else {
		red = 329.698727446 * pow(t - 60, -0.1332047592);
		green = 288.1221695283 * pow(t - 60, -0.0755148492);
	}
	if (t >= 66) {
		blue = 255;
	} else if (t <= 19) {
		blue = 0;
	} else {
		blue = 138.5177312231 * log(t - 10) - 305.0447927307;
	}
	*r = fmin(fmax(red, 0), 255) / 255.0;
	*g = fmin(fmax(green, 0), 255) / 255.0;
	*b = fmin(fmax(blue, 0), 255) / 255.0;
}

static bool scheduled_on(struct ts_server *server) {
	const char *mode = server->config.night_light;
	if (strcmp(mode, "always") == 0) {
		return true;
	}
	if (strcmp(mode, "scheduled") != 0) {
		return false;
	}
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	int minute = tm.tm_hour * 60 + tm.tm_min;
	int from = server->config.night_from, to = server->config.night_to;
	if (from == to) {
		return false;
	}
	if (from < to) {
		return minute >= from && minute < to;
	}
	return minute >= from || minute < to; /* across midnight */
}

void night_light_apply_output(struct ts_output *output) {
	struct ts_server *server = output->server;
	if (output->dpms_off || !output->wlr_output->enabled) {
		return;
	}
	size_t size = wlr_output_get_gamma_size(output->wlr_output);
	if (size == 0) {
		return;
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (server->night_active) {
		uint16_t *ramps = calloc(size * 3, sizeof(uint16_t));
		if (ramps == NULL) {
			wlr_output_state_finish(&state);
			return;
		}
		double rf, gf, bf;
		temperature_to_rgb(server->config.night_temperature, &rf, &gf, &bf);
		double nr, ng, nb;
		temperature_to_rgb(6500, &nr, &ng, &nb);
		rf /= nr;
		gf /= ng;
		bf /= nb;
		for (size_t i = 0; i < size; i++) {
			double v = (double)i / (size - 1) * 65535.0;
			ramps[i] = (uint16_t)fmin(v * rf, 65535.0);
			ramps[size + i] = (uint16_t)fmin(v * gf, 65535.0);
			ramps[2 * size + i] = (uint16_t)fmin(v * bf, 65535.0);
		}
		wlr_output_state_set_gamma_lut(&state, size, ramps, ramps + size, ramps + 2 * size);
		free(ramps);
	} else {
		wlr_output_state_set_gamma_lut(&state, 0, NULL, NULL, NULL);
	}
	if (!wlr_output_commit_state(output->wlr_output, &state)) {
		wlr_log(WLR_INFO, "Night light is not supported on %s", output->wlr_output->name);
	}
	wlr_output_state_finish(&state);
}

static void apply_all(struct ts_server *server) {
	struct ts_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		night_light_apply_output(output);
	}
}

void night_light_update(struct ts_server *server, bool force) {
	bool scheduled = scheduled_on(server);
	if (scheduled != server->night_scheduled) {
		/* The schedule moved on: a manual choice no longer applies. */
		server->night_override = 0;
		server->night_scheduled = scheduled;
	}
	bool want = server->night_override ? server->night_override > 0 : scheduled;
	if (want != server->night_active || force) {
		server->night_active = want;
		apply_all(server);
		ipc_send_state(server);
	}
}

void night_light_set(struct ts_server *server, bool on) {
	server->night_override = on ? 1 : -1;
	night_light_update(server, false);
}

static int night_tick(void *data) {
	struct ts_server *server = data;
	night_light_update(server, false);
	wl_event_source_timer_update(server->night_timer, 30 * 1000);
	return 0;
}

void night_light_init(struct ts_server *server) {
	server->night_scheduled = scheduled_on(server);
	server->night_active = server->night_scheduled;
	server->night_timer = wl_event_loop_add_timer(server->loop, night_tick, server);
	wl_event_source_timer_update(server->night_timer, 30 * 1000);
	apply_all(server);
}
