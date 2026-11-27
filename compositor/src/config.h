#ifndef TESSERA_CONFIG_H
#define TESSERA_CONFIG_H

#include <stdbool.h>

/* The settings, read from /etc/tessera/config and then the user's own
 * ~/.config/tessera/config. The format is described at the top of
 * /etc/tessera/config (data/config in the source). */

#define TS_MAX_DISPLAYS 8

/* The scales a screen can be asked for. Below the first is unreadably small
 * and above the second is not a screen anyone is looking at; both are checked
 * here so the global setting and a screen's own agree. 0 means "work it out". */
#define TS_SCALE_MIN 0.5
#define TS_SCALE_MAX 4.0

/* Where a screen goes: next to the main screen, at a spot, or showing the
 * same picture as the main screen. */
enum ts_display_place {
	TS_PLACE_AUTO,
	TS_PLACE_RIGHT,
	TS_PLACE_LEFT,
	TS_PLACE_ABOVE,
	TS_PLACE_BELOW,
	TS_PLACE_AT,
	TS_PLACE_MIRROR,
};

/* One screen's settings (display.PORT.* in the config file), by the port
 * it is plugged into, such as eDP-1 or HDMI-A-1. */
struct ts_display_config {
	char name[32];
	bool off;
	int width, height;          /* 0: the screen's own best size */
	int refresh;                /* mHz; 0: the fastest at that size */
	double scale;               /* 0: automatic */
	enum ts_display_place place;
	int x, y;                   /* for TS_PLACE_AT */
};

struct ts_config {
	char theme[16];
	char accent[16];
	char kb_layout[64];
	char kb_variant[64];
	char kb_options[128];
	char kb_model[64];
	bool tap_to_click;
	bool natural_scroll;
	double pointer_speed;  /* -1 (slow) to 1 (fast) */
	bool gestures;         /* three-finger swipes */
	bool swipe_reversed;
	bool animations;
	double scale;          /* 0 = automatic */
	int gaps;              /* -1 = follow the layout size */
	char layout_size[16];  /* auto, compact or comfortable */
	int idle_lock_minutes;
	int idle_screen_off_minutes;
	char night_light[16];  /* off, scheduled or always */
	int night_from;        /* minutes after midnight */
	int night_to;
	int night_temperature; /* kelvin */
	char wallpaper[512];   /* none, mosaic, or a PNG file */
	char terminal[256];
	char files[256];
	char browser[256];
	struct ts_display_config displays[TS_MAX_DISPLAYS];
	int display_count;
};

/* Tessera's defaults. */
void config_defaults(struct ts_config *c);
/* Debian's keyboard choice (/etc/default/keyboard). */
void config_read_keyboard(struct ts_config *c, const char *path);
/* One settings file on top of what is there; a missing file is skipped. */
void config_read_file(struct ts_config *c, const char *path);
/* Keeps numbers in their sensible ranges. */
void config_limit(struct ts_config *c);

#endif
