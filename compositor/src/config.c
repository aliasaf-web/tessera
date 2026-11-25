#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "server.h"

void config_load(struct ts_server *server) {
	struct ts_config *c = &server->config;
	config_defaults(c);
	/* Debian stores the keyboard chosen during installation here. */
	config_read_keyboard(c, "/etc/default/keyboard");
	config_read_file(c, "/etc/tessera/config");

	char path[512];
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (xdg && xdg[0]) {
		snprintf(path, sizeof(path), "%s/tessera/config", xdg);
	} else {
		snprintf(path, sizeof(path), "%s/.config/tessera/config", home ? home : "");
	}
	config_read_file(c, path);
	config_limit(c);
}

/* #RRGGBB or #RRGGBBAA; anything else (such as a mistyped code) gets the
 * fallback. The shell accepts the same (theme.py). */
static struct ts_color hex(const char *s, struct ts_color fallback) {
	size_t len = s ? strlen(s) : 0;
	if ((len != 7 && len != 9) || s[0] != '#' ||
			strspn(s + 1, "0123456789abcdefABCDEF") != len - 1) {
		return fallback;
	}
	unsigned r, g, b, a = 255;
	sscanf(s + 1, "%02x%02x%02x", &r, &g, &b);
	if (len == 9) {
		sscanf(s + 7, "%02x", &a);
	}
	return (struct ts_color){ r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f };
}

/* Named accent colours, tuned for dark and light backgrounds. The same
 * table is used by the shell (theme.py). */
static const struct {
	const char *name, *dark, *light;
} accents[] = {
	{ "orange", "#f0883e", "#c95a22" },
	{ "blue", "#5b9cf5", "#2f6fd6" },
	{ "teal", "#3cc8b4", "#13897a" },
	{ "green", "#57c272", "#2e8b47" },
	{ "yellow", "#e8b931", "#a87b00" },
	{ "red", "#f0605a", "#c83a35" },
	{ "pink", "#ec7fb2", "#c24d86" },
	{ "purple", "#a77bff", "#7a4de0" },
	{ "slate", "#8fa3b8", "#5a6b7d" },
};

static struct ts_color accent_color(const char *value, bool dark) {
	struct ts_color fallback = hex(dark ? "#f0883e" : "#c95a22", (struct ts_color){0, 0, 0, 1});
	for (size_t i = 0; i < sizeof(accents) / sizeof(accents[0]); i++) {
		if (strcmp(value, accents[i].name) == 0) {
			return hex(dark ? accents[i].dark : accents[i].light, fallback);
		}
	}
	return hex(value, fallback);
}

void theme_from_config(struct ts_server *server) {
	struct ts_theme *t = &server->theme;
	bool dark = strcmp(server->config.theme, "light") != 0;
	t->dark = dark;
	struct ts_color none = {0, 0, 0, 1};
	if (dark) {
		t->background = hex("#0f1012", none);
		t->surface = hex("#1a1b1e", none);
		t->surface_hover = hex("#2c2d32", none);
		t->text = hex("#ededee", none);
		t->text_dim = hex("#8d9099", none);
	} else {
		t->background = hex("#e7e9ed", none);
		t->surface = hex("#ffffff", none);
		t->surface_hover = hex("#eceef2", none);
		t->text = hex("#1b1d22", none);
		t->text_dim = hex("#5f636d", none);
	}
	t->accent = accent_color(server->config.accent, dark);
	/* Pick black or white text for things drawn on the accent colour. */
	struct ts_color a = t->accent;
	double lum = 0.2126 * a.r + 0.7152 * a.g + 0.0722 * a.b;
	t->on_accent = lum > 0.55 ? hex("#111111", none) : hex("#ffffff", none);
}

bool output_is_compact(struct ts_output *output) {
	const char *size = output->server->config.layout_size;
	if (strcmp(size, "compact") == 0) {
		return true;
	}
	if (strcmp(size, "comfortable") == 0) {
		return false;
	}
	return output->full_area.height > 0 && output->full_area.height <= 800;
}

int output_gaps(struct ts_output *output) {
	int gaps = output->server->config.gaps;
	if (gaps >= 0) {
		return gaps;
	}
	return output_is_compact(output) ? 8 : 14;
}
