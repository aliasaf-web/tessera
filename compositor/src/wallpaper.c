#define _POSIX_C_SOURCE 200809L
#include <cairo.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>
#include "server.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* The desktop background: plain colour (default), a quiet mosaic pattern
 * drawn here, or the user's own picture (a PNG prepared by Settings). */

static void set_color(cairo_t *cr, struct ts_color c, double alpha) {
	cairo_set_source_rgba(cr, c.r, c.g, c.b, alpha);
}

/* Small deterministic random generator so the pattern never changes. */
static uint32_t next_random(uint32_t *state) {
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

static void rounded_square(cairo_t *cr, double x, double y, double size, double r) {
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + size - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + size - r, y + size - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + size - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

/* Soft rounded tiles in offset rows, like a mosaic, fading away towards
 * the middle of the screen, over a faint glow of the accent colour. */
static void draw_mosaic(cairo_t *cr, struct ts_theme *theme, double w, double h) {
	set_color(cr, theme->background, 1);
	cairo_paint(cr);

	cairo_pattern_t *glow = cairo_pattern_create_radial(w * 0.08, h * 1.05, 0,
		w * 0.08, h * 1.05, fmax(w, h) * 0.75);
	struct ts_color a = theme->accent;
	cairo_pattern_add_color_stop_rgba(glow, 0, a.r, a.g, a.b, theme->dark ? 0.10 : 0.12);
	cairo_pattern_add_color_stop_rgba(glow, 1, a.r, a.g, a.b, 0);
	cairo_set_source(cr, glow);
	cairo_paint(cr);
	cairo_pattern_destroy(glow);

	const double tile = 64, gap = 6;
	uint32_t seed = 0x7e55e7a;
	double strength = theme->dark ? 0.045 : 0.06;
	int row = 0;
	for (double y = -tile / 2; y < h; y += tile, row++) {
		double shift = (row % 2) ? tile / 2 : 0;
		for (double x = -tile + shift; x < w; x += tile) {
			double r = (next_random(&seed) % 1000) / 1000.0;
			double cx = (x + tile / 2) / w - 0.5, cy = (y + tile / 2) / h - 0.5;
			double distance = sqrt(cx * cx * 1.4 + cy * cy);
			double fade = fmin(1.0, fmax(0.0, (distance - 0.18) / 0.42));
			double alpha = (0.35 + 0.65 * r) * strength * fade;
			if (alpha < 0.003) {
				continue;
			}
			if (theme->dark) {
				cairo_set_source_rgba(cr, 1, 1, 1, alpha);
			} else {
				cairo_set_source_rgba(cr, 0, 0, 0, alpha);
			}
			rounded_square(cr, x + gap / 2, y + gap / 2, tile - gap, 7);
			cairo_fill(cr);
		}
	}
}

static bool draw_picture(cairo_t *cr, const char *path, double w, double h) {
	cairo_surface_t *image = cairo_image_surface_create_from_png(path);
	if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(image);
		wlr_log(WLR_INFO, "Could not load the wallpaper %s", path);
		return false;
	}
	double iw = cairo_image_surface_get_width(image);
	double ih = cairo_image_surface_get_height(image);
	/* Cover the screen, cropping the middle of the picture. */
	double s = fmax(w / iw, h / ih);
	cairo_save(cr);
	cairo_translate(cr, (w - iw * s) / 2, (h - ih * s) / 2);
	cairo_scale(cr, s, s);
	cairo_set_source_surface(cr, image, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_restore(cr);
	cairo_surface_destroy(image);
	return true;
}

void wallpaper_output_destroyed(struct ts_output *output) {
	if (output->wallpaper) {
		wlr_scene_node_destroy(&output->wallpaper->node);
		output->wallpaper = NULL;
	}
	if (output->wallpaper_surface) {
		cairo_surface_destroy(output->wallpaper_surface);
		output->wallpaper_surface = NULL;
	}
	output->wallpaper_w = output->wallpaper_h = 0;
}

void wallpaper_output(struct ts_output *output) {
	struct ts_server *server = output->server;
	wallpaper_output_destroyed(output);
	const char *mode = server->config.wallpaper;
	struct wlr_box area = output->full_area;
	if (strcmp(mode, "none") == 0 || area.width <= 0 || area.height <= 0) {
		return;
	}
	double scale = output->wlr_output->scale;
	int pw = (int)ceil(area.width * scale), ph = (int)ceil(area.height * scale);
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return;
	}
	cairo_t *cr = cairo_create(surface);
	cairo_scale(cr, scale, scale);
	bool ok;
	if (strcmp(mode, "mosaic") == 0) {
		draw_mosaic(cr, &server->theme, area.width, area.height);
		ok = true;
	} else {
		set_color(cr, server->theme.background, 1);
		cairo_paint(cr);
		ok = draw_picture(cr, mode, area.width, area.height);
	}
	cairo_destroy(cr);
	cairo_surface_flush(surface);
	if (!ok) {
		cairo_surface_destroy(surface);
		return;
	}

	struct wlr_buffer *buffer = deco_buffer_from_surface(surface);
	if (buffer == NULL) {
		cairo_surface_destroy(surface);
		return;
	}
	output->wallpaper = wlr_scene_buffer_create(server->layer_background, buffer);
	wlr_buffer_drop(buffer);
	wlr_scene_buffer_set_dest_size(output->wallpaper, area.width, area.height);
	wlr_scene_node_set_position(&output->wallpaper->node, area.x, area.y);
	output->wallpaper_surface = surface; /* keeps our own reference */
	output->wallpaper_w = area.width;
	output->wallpaper_h = area.height;
	output->wallpaper_scale = scale;
}

void wallpaper_update(struct ts_server *server) {
	struct ts_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		wallpaper_output(output);
	}
}
