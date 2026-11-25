/* Reads tests/config-sample and checks every value. The shell's reader is
 * checked against the same file in tests/test_shell.py. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/config.h"

static int failures;

static void check(int ok, const char *what) {
	if (!ok) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

static const struct ts_display_config *display(const struct ts_config *c, const char *name) {
	for (int i = 0; i < c->display_count; i++) {
		if (strcmp(c->displays[i].name, name) == 0) {
			return &c->displays[i];
		}
	}
	return NULL;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		printf("usage: config_test SAMPLE\n");
		return 2;
	}
	struct ts_config c;
	config_defaults(&c);
	config_read_file(&c, argv[1]);
	config_limit(&c);

	check(strcmp(c.theme, "light") == 0, "quotes are removed");
	check(strcmp(c.accent, "purple") == 0, "the last value wins, single quotes removed");
	check(strcmp(c.terminal, "foot --app-id test") == 0, "spaces around are removed");
	check(strcmp(c.wallpaper, "/home/me/My Pictures/sea.png") == 0, "spaces inside are kept");
	check(!c.gestures, "false");
	check(fabs(c.scale - 1.25) < 1e-9, "scale");
	check(c.gaps == 12, "gaps");
	check(c.night_from == 21 * 60 + 30, "time");
	check(c.idle_lock_minutes == 5, "an unset value keeps its default");

	const struct ts_display_config *hdmi = display(&c, "HDMI-A-1");
	check(hdmi != NULL, "display.HDMI-A-1");
	if (hdmi) {
		check(hdmi->width == 1920 && hdmi->height == 1080, "mode size");
		check(hdmi->refresh == 59940, "refresh in mHz");
		check(hdmi->place == TS_PLACE_LEFT, "position left");
		check(fabs(hdmi->scale - 1.5) < 1e-9, "display scale");
		check(!hdmi->off, "on unless switched off");
	}
	const struct ts_display_config *edp = display(&c, "eDP-1");
	check(edp && edp->off, "enabled=false");
	const struct ts_display_config *dp = display(&c, "DP-2");
	check(dp && dp->place == TS_PLACE_AT && dp->x == 100 && dp->y == -50, "position X,Y");

	struct ts_config missing;
	config_defaults(&missing);
	config_read_file(&missing, "/nonexistent/tessera/config");
	check(strcmp(missing.theme, "dark") == 0, "a missing file changes nothing");

	if (failures == 0) {
		printf("all settings read correctly\n");
	}
	return failures ? 1 : 0;
}
