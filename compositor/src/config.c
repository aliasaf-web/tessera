#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>
#include "server.h"

static void copy_str(char *dst, size_t size, const char *src) {
	snprintf(dst, size, "%s", src);
}

static char *trim(char *s) {
	while (isspace((unsigned char)*s)) {
		s++;
	}
	char *end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1])) {
		*--end = '\0';
	}
	/* Strip matching quotes, as used in /etc/default/keyboard. */
	size_t len = strlen(s);
	if (len >= 2 && (s[0] == '"' || s[0] == '\'') && s[len - 1] == s[0]) {
		s[len - 1] = '\0';
		s++;
	}
	return s;
}

static bool parse_bool(const char *v) {
	return strcmp(v, "true") == 0 || strcmp(v, "yes") == 0 || strcmp(v, "1") == 0 ||
		strcmp(v, "on") == 0;
}

/* "HH:MM" to minutes after midnight; -1 if invalid. */
static int parse_time(const char *v) {
	int h, m;
	if (sscanf(v, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
		return -1;
	}
	return h * 60 + m;
}

/* Debian stores the keyboard chosen during installation here. */
static void load_system_keyboard(struct ts_config *c) {
	FILE *f = fopen("/etc/default/keyboard", "r");
	if (f == NULL) {
		return;
	}
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (eq == NULL || line[0] == '#') {
			continue;
		}
		*eq = '\0';
		char *key = trim(line);
		char *val = trim(eq + 1);
		if (strcmp(key, "XKBLAYOUT") == 0) {
			copy_str(c->kb_layout, sizeof(c->kb_layout), val);
		} else if (strcmp(key, "XKBVARIANT") == 0) {
			copy_str(c->kb_variant, sizeof(c->kb_variant), val);
		} else if (strcmp(key, "XKBOPTIONS") == 0) {
			copy_str(c->kb_options, sizeof(c->kb_options), val);
		} else if (strcmp(key, "XKBMODEL") == 0) {
			copy_str(c->kb_model, sizeof(c->kb_model), val);
		}
	}
	fclose(f);
}

static void load_file(struct ts_config *c, const char *path) {
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return;
	}
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line);
		if (s[0] == '#' || s[0] == '\0') {
			continue;
		}
		char *eq = strchr(s, '=');
		if (eq == NULL) {
			continue;
		}
		*eq = '\0';
		char *key = trim(s);
		char *val = trim(eq + 1);
		if (strcmp(key, "theme") == 0) {
			copy_str(c->theme, sizeof(c->theme), val);
		} else if (strcmp(key, "accent") == 0) {
			copy_str(c->accent, sizeof(c->accent), val);
		} else if (strcmp(key, "keyboard_layout") == 0 && val[0]) {
			copy_str(c->kb_layout, sizeof(c->kb_layout), val);
		} else if (strcmp(key, "keyboard_variant") == 0 && val[0]) {
			copy_str(c->kb_variant, sizeof(c->kb_variant), val);
		} else if (strcmp(key, "keyboard_options") == 0 && val[0]) {
			copy_str(c->kb_options, sizeof(c->kb_options), val);
		} else if (strcmp(key, "tap_to_click") == 0) {
			c->tap_to_click = parse_bool(val);
		} else if (strcmp(key, "natural_scroll") == 0) {
			c->natural_scroll = parse_bool(val);
		} else if (strcmp(key, "pointer_speed") == 0) {
			c->pointer_speed = atof(val);
		} else if (strcmp(key, "gestures") == 0) {
			c->gestures = parse_bool(val);
		} else if (strcmp(key, "swipe_direction") == 0) {
			c->swipe_reversed = strcmp(val, "reversed") == 0;
		} else if (strcmp(key, "animations") == 0) {
			c->animations = parse_bool(val);
		} else if (strcmp(key, "scale") == 0) {
			c->scale = strcmp(val, "auto") == 0 ? 0 : atof(val);
		} else if (strcmp(key, "gaps") == 0) {
			c->gaps = val[0] ? atoi(val) : -1;
		} else if (strcmp(key, "layout_size") == 0) {
			copy_str(c->layout_size, sizeof(c->layout_size), val);
		} else if (strcmp(key, "idle_lock_minutes") == 0) {
			c->idle_lock_minutes = atoi(val);
		} else if (strcmp(key, "idle_screen_off_minutes") == 0) {
			c->idle_screen_off_minutes = atoi(val);
		} else if (strcmp(key, "lock_on_start") == 0) {
			c->lock_on_start = parse_bool(val);
		} else if (strcmp(key, "night_light") == 0) {
			copy_str(c->night_light, sizeof(c->night_light), val);
		} else if (strcmp(key, "night_light_from") == 0) {
			int t = parse_time(val);
			if (t >= 0) {
				c->night_from = t;
			}
		} else if (strcmp(key, "night_light_to") == 0) {
			int t = parse_time(val);
			if (t >= 0) {
				c->night_to = t;
			}
		} else if (strcmp(key, "night_light_temperature") == 0) {
			c->night_temperature = atoi(val);
		} else if (strcmp(key, "wallpaper") == 0) {
			copy_str(c->wallpaper, sizeof(c->wallpaper), val[0] ? val : "none");
		} else if (strcmp(key, "terminal") == 0) {
			copy_str(c->terminal, sizeof(c->terminal), val);
		} else if (strcmp(key, "files") == 0) {
			copy_str(c->files, sizeof(c->files), val);
		} else if (strcmp(key, "browser") == 0) {
			copy_str(c->browser, sizeof(c->browser), val);
		}
	}
	fclose(f);
}

void config_load(struct ts_server *server) {
	struct ts_config *c = &server->config;
	memset(c, 0, sizeof(*c));
	copy_str(c->theme, sizeof(c->theme), "dark");
	copy_str(c->kb_layout, sizeof(c->kb_layout), "gb");
	c->tap_to_click = true;
	c->natural_scroll = false;
	c->pointer_speed = 0;
	c->gestures = true;
	c->swipe_reversed = false;
	c->animations = true;
	c->scale = 0;
	c->gaps = -1;
	copy_str(c->layout_size, sizeof(c->layout_size), "auto");
	c->idle_lock_minutes = 5;
	c->idle_screen_off_minutes = 10;
	c->lock_on_start = false;
	copy_str(c->night_light, sizeof(c->night_light), "off");
	c->night_from = 20 * 60;
	c->night_to = 7 * 60;
	c->night_temperature = 4000;
	copy_str(c->wallpaper, sizeof(c->wallpaper), "none");
	copy_str(c->terminal, sizeof(c->terminal), "x-terminal-emulator");
	copy_str(c->files, sizeof(c->files), "xdg-open \"$HOME\"");
	copy_str(c->browser, sizeof(c->browser), "x-www-browser");

	load_system_keyboard(c);
	load_file(c, "/etc/tessera/config");

	char path[512];
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (xdg && xdg[0]) {
		snprintf(path, sizeof(path), "%s/tessera/config", xdg);
	} else {
		snprintf(path, sizeof(path), "%s/.config/tessera/config", home ? home : "");
	}
	load_file(c, path);

	if (c->gaps > 64) {
		c->gaps = 64;
	}
	if (c->pointer_speed < -1) {
		c->pointer_speed = -1;
	} else if (c->pointer_speed > 1) {
		c->pointer_speed = 1;
	}
	if (c->night_temperature < 2000) {
		c->night_temperature = 2000;
	} else if (c->night_temperature > 6500) {
		c->night_temperature = 6500;
	}
}

static struct ts_color hex(const char *s, struct ts_color fallback) {
	if (s == NULL || s[0] != '#' || (strlen(s) != 7 && strlen(s) != 9)) {
		return fallback;
	}
	unsigned r, g, b, a = 255;
	if (sscanf(s + 1, "%02x%02x%02x", &r, &g, &b) != 3) {
		return fallback;
	}
	if (strlen(s) == 9) {
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
