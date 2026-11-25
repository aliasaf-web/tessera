#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

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

void config_read_keyboard(struct ts_config *c, const char *path) {
	FILE *f = fopen(path, "r");
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

static struct ts_display_config *display_entry(struct ts_config *c, const char *name) {
	for (int i = 0; i < c->display_count; i++) {
		if (strcmp(c->displays[i].name, name) == 0) {
			return &c->displays[i];
		}
	}
	if (c->display_count == TS_MAX_DISPLAYS) {
		return NULL;
	}
	struct ts_display_config *d = &c->displays[c->display_count++];
	memset(d, 0, sizeof(*d));
	copy_str(d->name, sizeof(d->name), name);
	return d;
}

/* display.PORT.enabled = true | false
 * display.PORT.mode     = auto | WIDTHxHEIGHT | WIDTHxHEIGHT@HZ
 * display.PORT.scale    = auto | 1 | 1.25 | ...
 * display.PORT.position = auto | right | left | above | below | mirror | X,Y
 * right, left, above and below are next to the main screen (the built-in
 * one if it is in use). */
static void parse_display(struct ts_config *c, const char *key, const char *val) {
	const char *port = key + strlen("display.");
	const char *dot = strrchr(port, '.');
	if (dot == NULL || dot == port || (size_t)(dot - port) >= sizeof(c->displays[0].name)) {
		return;
	}
	char name[sizeof(c->displays[0].name)];
	memcpy(name, port, dot - port);
	name[dot - port] = '\0';
	const char *field = dot + 1;
	struct ts_display_config *d = display_entry(c, name);
	if (d == NULL) {
		return;
	}
	if (strcmp(field, "enabled") == 0) {
		d->off = !parse_bool(val);
	} else if (strcmp(field, "mode") == 0) {
		int w = 0, h = 0;
		double hz = 0;
		d->width = d->height = d->refresh = 0;
		if (sscanf(val, "%dx%d@%lf", &w, &h, &hz) >= 2 && w > 0 && h > 0) {
			d->width = w;
			d->height = h;
			d->refresh = hz > 0 ? (int)(hz * 1000 + 0.5) : 0;
		}
	} else if (strcmp(field, "scale") == 0) {
		double scale = strcmp(val, "auto") == 0 ? 0 : atof(val);
		d->scale = scale >= 0.5 && scale <= 4 ? scale : 0;
	} else if (strcmp(field, "position") == 0) {
		static const struct {
			const char *word;
			enum ts_display_place place;
		} words[] = {
			{ "right", TS_PLACE_RIGHT }, { "left", TS_PLACE_LEFT },
			{ "above", TS_PLACE_ABOVE }, { "below", TS_PLACE_BELOW },
			{ "mirror", TS_PLACE_MIRROR },
		};
		d->place = TS_PLACE_AUTO;
		for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
			if (strcmp(val, words[i].word) == 0) {
				d->place = words[i].place;
			}
		}
		int x, y;
		if (d->place == TS_PLACE_AUTO && sscanf(val, "%d,%d", &x, &y) == 2) {
			d->place = TS_PLACE_AT;
			d->x = x;
			d->y = y;
		}
	}
}

void config_read_file(struct ts_config *c, const char *path) {
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
		} else if (strncmp(key, "display.", strlen("display.")) == 0) {
			parse_display(c, key, val);
		}
	}
	fclose(f);
}

void config_defaults(struct ts_config *c) {
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
	copy_str(c->night_light, sizeof(c->night_light), "off");
	c->night_from = 20 * 60;
	c->night_to = 7 * 60;
	c->night_temperature = 4000;
	copy_str(c->wallpaper, sizeof(c->wallpaper), "none");
	copy_str(c->terminal, sizeof(c->terminal), "x-terminal-emulator");
	copy_str(c->files, sizeof(c->files), "xdg-open \"$HOME\"");
	copy_str(c->browser, sizeof(c->browser), "x-www-browser");
}

void config_limit(struct ts_config *c) {
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
