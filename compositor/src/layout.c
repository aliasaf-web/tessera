#define _POSIX_C_SOURCE 200809L
#include <limits.h>
#include <stdlib.h>
#include "server.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

bool view_visible(struct ts_view *view) {
	return view->mapped && view->output && view->workspace == view->output->workspace;
}

static bool view_is_tiled(struct ts_view *view) {
	return !view->floating && !view->fullscreen && !view->maximized;
}

static struct wlr_box tile_area(struct ts_output *output) {
	int g = output->server->config.gaps;
	struct wlr_box area = output->usable_area;
	area.x += g;
	area.y += g;
	area.width = MAX(area.width - 2 * g, 50);
	area.height = MAX(area.height - 2 * g, 50);
	return area;
}

static int count_tiled(struct ts_output *output) {
	int n = 0;
	struct ts_view *view;
	wl_list_for_each(view, &output->server->views, link) {
		if (view->output == output && view_visible(view) && view_is_tiled(view)) {
			n++;
		}
	}
	return n;
}

static int master_width(struct ts_output *output, struct wlr_box area) {
	int g = output->server->config.gaps;
	return (int)((area.width - g) * output->master_ratio[output->workspace]);
}

void arrange_output(struct ts_output *output) {
	struct ts_server *server = output->server;
	int g = server->config.gaps;
	struct wlr_box area = tile_area(output);
	int n = count_tiled(output);
	int mw = master_width(output, area);
	int stack_n = n - 1;
	int i = 0;

	struct ts_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->output != output || !view->mapped) {
			continue;
		}
		bool visible = view_visible(view);
		wlr_scene_node_set_enabled(&view->tree->node, visible);
		wlr_scene_node_set_enabled(&view->popup_tree->node, visible);
		if (!visible) {
			continue;
		}
		if (view->fullscreen) {
			view_set_box(view, output->full_area);
			continue;
		}
		if (view->maximized) {
			view_set_box(view, area);
			continue;
		}
		if (view->floating) {
			/* Keep floating windows on screen. */
			struct wlr_box b = view->box;
			struct wlr_box u = output->usable_area;
			if (b.x + b.width < u.x + 60) {
				b.x = u.x;
			}
			if (b.x > u.x + u.width - 60) {
				b.x = u.x + u.width - b.width;
			}
			b.y = MAX(b.y, u.y);
			if (b.y > u.y + u.height - 40) {
				b.y = u.y + u.height - b.height;
			}
			view->box = b;
			view_update_frame(view);
			continue;
		}

		struct wlr_box box;
		if (n == 1) {
			box = area;
		} else if (i == 0) {
			box = (struct wlr_box){ area.x, area.y, mw, area.height };
		} else {
			int sx = area.x + mw + g;
			int sw = area.width - mw - g;
			int total = area.height - (stack_n - 1) * g;
			int idx = i - 1;
			int y0 = area.y + idx * (total / stack_n + g);
			int h = (idx == stack_n - 1) ? area.y + area.height - y0 : total / stack_n;
			box = (struct wlr_box){ sx, y0, sw, h };
		}
		i++;
		view_set_box(view, box);
	}
}

void arrange_all(struct ts_server *server) {
	struct ts_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		arrange_layers(output);
	}
}

void workspace_switch(struct ts_server *server, int workspace) {
	if (workspace < 1 || workspace > TS_WORKSPACES) {
		return;
	}
	struct ts_output *output = output_focused(server);
	if (output == NULL || output->workspace == workspace) {
		return;
	}
	output->workspace = workspace;
	arrange_output(output);
	server->focused_view = NULL;
	focus_top_view(server);
	cursor_rebase(server);
	ipc_send_state(server);
}

struct ts_view *view_in_direction(struct ts_server *server, struct ts_view *from,
		int dx, int dy) {
	if (from == NULL) {
		return NULL;
	}
	double fx = from->box.x + from->box.width / 2.0;
	double fy = from->box.y + from->box.height / 2.0;
	struct ts_view *best = NULL;
	double best_score = 1e18;
	struct ts_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view == from || !view_visible(view) || view->output != from->output) {
			continue;
		}
		double cx = view->box.x + view->box.width / 2.0;
		double cy = view->box.y + view->box.height / 2.0;
		double ddx = cx - fx, ddy = cy - fy;
		double along = ddx * dx + ddy * dy;
		if (along <= 0) {
			continue;
		}
		double across = dx ? (ddy < 0 ? -ddy : ddy) : (ddx < 0 ? -ddx : ddx);
		double score = along + across * 2;
		if (score < best_score) {
			best_score = score;
			best = view;
		}
	}
	return best;
}

void view_swap(struct ts_view *a, struct ts_view *b) {
	if (a == NULL || b == NULL || a == b) {
		return;
	}
	struct wl_list *a_prev = a->link.prev;
	struct wl_list *b_prev = b->link.prev;
	if (a_prev == &b->link) {
		/* b directly before a */
		wl_list_remove(&b->link);
		wl_list_insert(&a->link, &b->link);
	} else if (b_prev == &a->link) {
		wl_list_remove(&a->link);
		wl_list_insert(&b->link, &a->link);
	} else {
		wl_list_remove(&a->link);
		wl_list_remove(&b->link);
		wl_list_insert(b_prev, &a->link);
		wl_list_insert(a_prev, &b->link);
	}
	if (a->output) {
		arrange_output(a->output);
	}
	if (b->output && b->output != a->output) {
		arrange_output(b->output);
	}
}

bool layout_gap_hit(struct ts_output *output, double lx, double ly) {
	if (output == NULL || count_tiled(output) < 2) {
		return false;
	}
	struct ts_view *view;
	wl_list_for_each(view, &output->server->views, link) {
		if (view->output == output && view_visible(view) &&
				(view->fullscreen || view->maximized)) {
			return false;
		}
	}
	int g = output->server->config.gaps;
	struct wlr_box area = tile_area(output);
	int left = area.x + master_width(output, area);
	int pad = g < 8 ? 4 : 0;
	return lx >= left - pad && lx <= left + g + pad &&
		ly >= area.y && ly <= area.y + area.height;
}

void layout_set_ratio_from_cursor(struct ts_output *output, double lx) {
	int g = output->server->config.gaps;
	struct wlr_box area = tile_area(output);
	double ratio = (lx - area.x - g / 2.0) / (double)(area.width - g);
	if (ratio < 0.2) {
		ratio = 0.2;
	} else if (ratio > 0.8) {
		ratio = 0.8;
	}
	output->master_ratio[output->workspace] = ratio;
	arrange_output(output);
}
