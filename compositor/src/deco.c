#define _POSIX_C_SOURCE 200809L
#include <cairo.h>
#include <drm_fourcc.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include "server.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TITLEBAR_HEIGHT 38
#define BORDER 2
#define RADIUS 12
#define CLOSE_RADIUS 13
#define CLOSE_MARGIN 8

int deco_titlebar_height(void) {
	return TITLEBAR_HEIGHT;
}

int deco_border(void) {
	return BORDER;
}

int deco_radius(void) {
	return RADIUS;
}

static double close_cx(int frame_w) {
	return frame_w - CLOSE_MARGIN - CLOSE_RADIUS;
}

bool deco_close_hit(int frame_w, double x, double y) {
	double dx = x - close_cx(frame_w);
	double dy = y - TITLEBAR_HEIGHT / 2.0;
	/* Generous hit area: a bit bigger than the drawn circle. */
	return dx * dx + dy * dy <= (CLOSE_RADIUS + 4) * (CLOSE_RADIUS + 4);
}

/* A wlr_buffer backed by a cairo image surface. */
struct cairo_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

static void cairo_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct cairo_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	cairo_surface_destroy(buffer->surface);
	free(buffer);
}

static bool cairo_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct cairo_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
		return false;
	}
	*data = cairo_image_surface_get_data(buffer->surface);
	*format = DRM_FORMAT_ARGB8888;
	*stride = cairo_image_surface_get_stride(buffer->surface);
	return true;
}

static void cairo_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
	(void)wlr_buffer;
}

static const struct wlr_buffer_impl cairo_buffer_impl = {
	.destroy = cairo_buffer_destroy,
	.begin_data_ptr_access = cairo_buffer_begin_data_ptr_access,
	.end_data_ptr_access = cairo_buffer_end_data_ptr_access,
};

static struct cairo_buffer *cairo_buffer_create(int width, int height) {
	if (width < 1) {
		width = 1;
	}
	if (height < 1) {
		height = 1;
	}
	struct cairo_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		return NULL;
	}
	buffer->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	if (cairo_surface_status(buffer->surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(buffer->surface);
		free(buffer);
		return NULL;
	}
	wlr_buffer_init(&buffer->base, &cairo_buffer_impl, width, height);
	return buffer;
}

/* Wrap an existing cairo image surface; the buffer holds its own reference. */
struct wlr_buffer *deco_buffer_from_surface(void *cairo_surface) {
	cairo_surface_t *surface = cairo_surface;
	struct cairo_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		return NULL;
	}
	buffer->surface = cairo_surface_reference(surface);
	wlr_buffer_init(&buffer->base, &cairo_buffer_impl, cairo_image_surface_get_width(surface),
		cairo_image_surface_get_height(surface));
	return &buffer->base;
}

static void set_color(cairo_t *cr, struct ts_color c) {
	cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
	if (r > w / 2) {
		r = w / 2;
	}
	if (r > h / 2) {
		r = h / 2;
	}
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

static PangoLayout *make_layout(cairo_t *cr, const char *text, const char *font, double px,
		int weight) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_font_description_set_absolute_size(desc, px * PANGO_SCALE);
	pango_font_description_set_weight(desc, weight);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text ? text : "", -1);
	pango_layout_set_single_paragraph_mode(layout, TRUE);
	return layout;
}

static void draw_text(cairo_t *cr, const char *text, const char *font, double px,
		int weight, double x, double y, double max_w, struct ts_color color,
		bool center, double *out_w, double *out_h) {
	PangoLayout *layout = make_layout(cr, text, font, px, weight);
	if (max_w > 0) {
		pango_layout_set_width(layout, (int)(max_w * PANGO_SCALE));
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	}
	int tw, th;
	pango_layout_get_pixel_size(layout, &tw, &th);
	set_color(cr, color);
	cairo_move_to(cr, center ? x - tw / 2.0 : x, y - th / 2.0);
	pango_cairo_show_layout(cr, layout);
	if (out_w) {
		*out_w = tw;
	}
	if (out_h) {
		*out_h = th;
	}
	g_object_unref(layout);
}

static void text_size(cairo_t *cr, const char *text, const char *font, double px,
		int weight, double *out_w, double *out_h) {
	PangoLayout *layout = make_layout(cr, text, font, px, weight);
	int tw, th;
	pango_layout_get_pixel_size(layout, &tw, &th);
	if (out_w) {
		*out_w = tw;
	}
	if (out_h) {
		*out_h = th;
	}
	g_object_unref(layout);
}

static struct ts_color subtle_border(struct ts_server *server) {
	return server->theme.dark ? (struct ts_color){1, 1, 1, 0.06f}
		: (struct ts_color){0, 0, 0, 0.10f};
}

struct wlr_buffer *deco_render_frame(struct ts_server *server, int w, int h, double scale,
		const char *title, bool focused, bool ssd, bool hover_close, bool floating) {
	struct ts_theme *t = &server->theme;
	struct cairo_buffer *buffer = cairo_buffer_create(ceil(w * scale), ceil(h * scale));
	if (buffer == NULL) {
		return NULL;
	}
	cairo_t *cr = cairo_create(buffer->surface);
	cairo_scale(cr, scale, scale);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

	rounded_rect(cr, 0, 0, w, h, RADIUS);
	set_color(cr, t->surface);
	cairo_fill(cr);

	if (ssd) {
		double max_title = w - 16 - (CLOSE_MARGIN + 2 * CLOSE_RADIUS) - 12;
		draw_text(cr, title, "Inter, sans-serif", 13, focused ? 560 : 500, 16,
			TITLEBAR_HEIGHT / 2.0, max_title, focused ? t->text : t->text_dim,
			false, NULL, NULL);

		double cx = close_cx(w), cy = TITLEBAR_HEIGHT / 2.0;
		if (hover_close) {
			cairo_arc(cr, cx, cy, CLOSE_RADIUS, 0, 2 * M_PI);
			set_color(cr, t->surface_hover);
			cairo_fill(cr);
		}
		double s = 4.5;
		cairo_set_line_width(cr, 1.7);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		set_color(cr, hover_close || focused ? t->text : t->text_dim);
		cairo_move_to(cr, cx - s, cy - s);
		cairo_line_to(cr, cx + s, cy + s);
		cairo_move_to(cr, cx + s, cy - s);
		cairo_line_to(cr, cx - s, cy + s);
		cairo_stroke(cr);
	}

	if (focused) {
		rounded_rect(cr, 1, 1, w - 2, h - 2, RADIUS - 1);
		set_color(cr, t->accent);
		cairo_set_line_width(cr, 2);
		cairo_stroke(cr);
	} else if (!t->dark || floating) {
		rounded_rect(cr, 0.5, 0.5, w - 1, h - 1, RADIUS - 0.5);
		set_color(cr, subtle_border(server));
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}

	cairo_destroy(cr);
	cairo_surface_flush(buffer->surface);
	return &buffer->base;
}

/* Corner masks sit on top of the window contents and paint the desktop
 * colour outside a quarter circle, which gives windows rounded corners
 * without any expensive clipping. corner: 0 top-left, 1 top-right,
 * 2 bottom-right, 3 bottom-left. */
/* Paint what lies behind a window at this spot: the wallpaper if there is
 * one, otherwise the desktop colour. ox/oy are the spot's position on the
 * screen in logical pixels. */
static void paint_backdrop(cairo_t *cr, struct ts_server *server, struct ts_output *output,
		double ox, double oy, double scale) {
	if (output && output->wallpaper_surface) {
		cairo_pattern_t *pattern = cairo_pattern_create_for_surface(output->wallpaper_surface);
		cairo_matrix_t matrix;
		cairo_matrix_init_scale(&matrix, scale, scale);
		cairo_matrix_translate(&matrix, ox, oy);
		cairo_pattern_set_matrix(pattern, &matrix);
		cairo_set_source(cr, pattern);
		cairo_pattern_destroy(pattern);
	} else {
		set_color(cr, server->theme.background);
	}
}

/* Corner masks sit on top of the window contents and paint the desktop
 * outside a quarter circle, which gives windows rounded corners without
 * any expensive clipping. corner: 0 top-left, 1 top-right, 2 bottom-right,
 * 3 bottom-left. */
struct wlr_buffer *deco_render_corner(struct ts_server *server, int corner, double scale,
		bool focused, struct ts_output *output, double ox, double oy) {
	struct ts_theme *t = &server->theme;
	int size = RADIUS;
	struct cairo_buffer *buffer = cairo_buffer_create(ceil(size * scale), ceil(size * scale));
	if (buffer == NULL) {
		return NULL;
	}
	cairo_t *cr = cairo_create(buffer->surface);
	cairo_scale(cr, scale, scale);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

	/* Centre of the circle in this tile's coordinates. */
	double cx = (corner == 0 || corner == 3) ? size : 0;
	double cy = (corner == 0 || corner == 1) ? size : 0;

	paint_backdrop(cr, server, output, ox, oy, scale);
	cairo_rectangle(cr, 0, 0, size, size);
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, cy, RADIUS, 0, 2 * M_PI);
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
	cairo_fill(cr);

	if (focused) {
		set_color(cr, t->accent);
		cairo_set_line_width(cr, 2);
		cairo_arc(cr, cx, cy, RADIUS - 1, 0, 2 * M_PI);
		cairo_stroke(cr);
	} else if (!t->dark) {
		set_color(cr, subtle_border(server));
		cairo_set_line_width(cr, 1);
		cairo_arc(cr, cx, cy, RADIUS - 0.5, 0, 2 * M_PI);
		cairo_stroke(cr);
	}

	cairo_destroy(cr);
	cairo_surface_flush(buffer->surface);
	return &buffer->base;
}

static const char *display_name(void) {
	static char name[128];
	struct passwd *pw = getpwuid(getuid());
	if (pw == NULL) {
		return "";
	}
	if (pw->pw_gecos && pw->pw_gecos[0] && pw->pw_gecos[0] != ',') {
		snprintf(name, sizeof(name), "%s", pw->pw_gecos);
		char *comma = strchr(name, ',');
		if (comma) {
			*comma = '\0';
		}
		if (name[0]) {
			return name;
		}
	}
	snprintf(name, sizeof(name), "%s", pw->pw_name);
	return name;
}

struct wlr_buffer *deco_render_lock(struct ts_server *server, struct ts_output *output,
		int w, int h, double scale) {
	struct ts_theme *t = &server->theme;
	struct cairo_buffer *buffer = cairo_buffer_create(ceil(w * scale), ceil(h * scale));
	if (buffer == NULL) {
		return NULL;
	}
	cairo_t *cr = cairo_create(buffer->surface);
	cairo_scale(cr, scale, scale);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

	set_color(cr, t->background);
	cairo_paint(cr);
	if (output && output->wallpaper_surface) {
		/* The wallpaper, dimmed so the text stays easy to read. */
		paint_backdrop(cr, server, output, 0, 0, scale);
		cairo_paint(cr);
		cairo_set_source_rgba(cr, t->background.r, t->background.g, t->background.b, 0.78);
		cairo_paint(cr);
	}

	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char clock_text[32], date_text[64];
	strftime(clock_text, sizeof(clock_text), "%H:%M", &tm);
	strftime(date_text, sizeof(date_text), "%A %-d %B", &tm);

	double cx = w / 2.0;
	double cy = h / 2.0 - 70;
	draw_text(cr, clock_text, "Inter, sans-serif", 96, 300, cx, cy, 0, t->text,
		true, NULL, NULL);
	draw_text(cr, date_text, "Inter, sans-serif", 20, 400, cx, cy + 74, 0, t->text_dim,
		true, NULL, NULL);

	/* Password field */
	double fw = 340, fh = 52;
	double fx = cx - fw / 2, fy = cy + 130;
	rounded_rect(cr, fx, fy, fw, fh, fh / 2);
	set_color(cr, t->surface);
	cairo_fill_preserve(cr);
	set_color(cr, server->auth_failed ? t->accent : subtle_border(server));
	cairo_set_line_width(cr, server->auth_failed ? 2 : 1);
	cairo_stroke(cr);

	if (server->password_len == 0) {
		char placeholder[200];
		snprintf(placeholder, sizeof(placeholder), "Password for %s", display_name());
		draw_text(cr, placeholder, "Inter, sans-serif", 15, 400, cx, fy + fh / 2,
			fw - 48, t->text_dim, true, NULL, NULL);
	} else {
		size_t dots = server->password_len > 20 ? 20 : server->password_len;
		double spacing = 14;
		double start = cx - (dots - 1) * spacing / 2.0;
		set_color(cr, t->text);
		for (size_t i = 0; i < dots; i++) {
			cairo_arc(cr, start + i * spacing, fy + fh / 2, 4, 0, 2 * M_PI);
			cairo_fill(cr);
		}
	}

	const char *hint = "Type your password, then press Enter";
	struct ts_color hint_color = t->text_dim;
	char wait[96];
	if (server->lock_armed == TS_LOCK_BUTTON_RESTART) {
		hint = "Click Restart again to restart. Open apps will close.";
		hint_color = t->accent;
	} else if (server->lock_armed == TS_LOCK_BUTTON_SHUT_DOWN) {
		hint = "Click Shut down again to turn off. Open apps will close.";
		hint_color = t->accent;
	} else if (server->auth_busy) {
		hint = "Checking…";
	} else if (server->auth_wait > 0) {
		snprintf(wait, sizeof(wait), "Too many wrong passwords. Try again in %d second%s.",
			server->auth_wait, server->auth_wait == 1 ? "" : "s");
		hint = wait;
		hint_color = t->accent;
	} else if (server->auth_failed) {
		hint = "That password didn't work. Please try again.";
		hint_color = t->accent;
	}
	draw_text(cr, hint, "Inter, sans-serif", 14, 400, cx, fy + fh + 30, w - 40,
		hint_color, true, NULL, NULL);

	/* Caps Lock, and the keyboard layout when there is more than one. */
	double note_y = fy + fh + 56;
	if (lock_caps_lock_on(server)) {
		draw_text(cr, "Caps Lock is on", "Inter, sans-serif", 13, 600, cx, note_y, 0,
			t->accent, true, NULL, NULL);
		note_y += 22;
	}
	const char *layout = lock_layout_name(server);
	if (layout) {
		char note[160];
		snprintf(note, sizeof(note), "Keyboard: %s", layout);
		draw_text(cr, note, "Inter, sans-serif", 13, 400, cx, note_y, w - 40, t->text_dim,
			true, NULL, NULL);
	}

	/* Sleep, Restart and Shut down along the bottom. */
	static const char *const labels[TS_LOCK_BUTTON_COUNT] = {
		[TS_LOCK_BUTTON_SLEEP] = "Sleep",
		[TS_LOCK_BUTTON_RESTART] = "Restart",
		[TS_LOCK_BUTTON_SHUT_DOWN] = "Shut down",
	};
	const double bh = 36, pad = 18, gap = 10;
	double widths[TS_LOCK_BUTTON_COUNT] = {0}, total = 0;
	for (int i = TS_LOCK_BUTTON_NONE + 1; i < TS_LOCK_BUTTON_COUNT; i++) {
		double tw;
		text_size(cr, labels[i], "Inter, sans-serif", 13, 500, &tw, NULL);
		widths[i] = tw + 2 * pad;
		total += widths[i] + (i > TS_LOCK_BUTTON_NONE + 1 ? gap : 0);
	}
	double bx = cx - total / 2, by = h - 28 - bh;
	for (int i = TS_LOCK_BUTTON_NONE + 1; i < TS_LOCK_BUTTON_COUNT; i++) {
		bool armed = server->lock_armed == (enum ts_lock_button)i;
		bool hover = server->lock_hover == (enum ts_lock_button)i;
		rounded_rect(cr, bx, by, widths[i], bh, bh / 2);
		set_color(cr, armed ? t->accent : (hover ? t->surface_hover : t->surface));
		cairo_fill(cr);
		draw_text(cr, labels[i], "Inter, sans-serif", 13, 500, bx + widths[i] / 2,
			by + bh / 2, 0, armed ? t->on_accent : t->text, true, NULL, NULL);
		if (output) {
			output->lock_buttons[i] = (struct wlr_box){
				(int)bx, (int)by, (int)widths[i], (int)bh };
		}
		bx += widths[i] + gap;
	}

	cairo_destroy(cr);
	cairo_surface_flush(buffer->surface);
	return &buffer->base;
}
