#define _POSIX_C_SOURCE 200809L
#include <limits.h>
#include <math.h>
#include <time.h>
#include <stdlib.h>
#include "server.h"
#include "tile_math.h"

bool view_visible(struct ts_view *view) {
	return view->mapped && view->output && view->workspace == view->output->workspace;
}

static bool view_is_tiled(struct ts_view *view) {
	return !view->floating && !view->fullscreen && !view->maximized;
}

static struct wlr_box tile_area(struct ts_output *output) {
	int g = output_gaps(output);
	struct wlr_box area = output->usable_area;
	area.x += g;
	area.y += g;
	area.width = MAX(area.width - 2 * g, 50);
	area.height = MAX(area.height - 2 * g, 50);
	return area;
}

int layout_count_tiled(struct ts_output *output) {
	int n = 0;
	struct ts_view *view;
	wl_list_for_each(view, &output->server->views, link) {
		if (view->output == output && view_visible(view) && view_is_tiled(view)) {
			n++;
		}
	}
	return n;
}

void arrange_output(struct ts_output *output) {
	struct ts_server *server = output->server;
	int g = output_gaps(output);
	double ratio = output->master_ratio[output->workspace];
	struct wlr_box area = tile_area(output);
	int n = layout_count_tiled(output);
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

		view_set_box(view, tile_slot(area, g, ratio, n, i++));
	}
	/* Which windows can be seen may have changed. */
	idle_update_inhibit(server);
}

void arrange_all(struct ts_server *server) {
	struct ts_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		arrange_layers(output);
	}
}

/* ---------------------------------------------------------------------- */
/* Workspaces and the slide between them                                    */
/* ---------------------------------------------------------------------- */

#define WS_ANIM_MS 220

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* How far the slide has carried this window: 0 for a window that is not part
 * of one, or is on another screen. */
int layout_slide_offset(struct ts_view *view) {
	struct ts_server *server = view->server;
	if (!server->ws_anim.active || view->output == NULL ||
			server->ws_anim.output != view->output) {
		return 0;
	}
	return slide_offset(server->ws_anim.dir, server->ws_anim.width, server->ws_anim.eased,
		view->workspace == server->ws_anim.from, view->workspace == server->ws_anim.to);
}

static void anim_place(struct ts_server *server, double progress) {
	struct ts_output *output = server->ws_anim.output;
	/* Kept on the server, not passed along: a window can commit while the
	 * slide is running, and it then asks where the slide has it. */
	server->ws_anim.eased = 1.0 - pow(1.0 - progress, 3);
	server->ws_anim.width = output->full_area.width + output_gaps(output);
	struct ts_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (!view->mapped || view->output != output ||
				(view->workspace != server->ws_anim.from &&
				 view->workspace != server->ws_anim.to)) {
			continue;
		}
		int offset = layout_slide_offset(view);
		wlr_scene_node_set_enabled(&view->tree->node, true);
		wlr_scene_node_set_enabled(&view->popup_tree->node, true);
		wlr_scene_node_set_position(&view->tree->node, view->box.x + offset, view->box.y);
		wlr_scene_node_set_position(&view->popup_tree->node,
			view->box.x + view->content.x + offset, view->box.y + view->content.y);
	}
}

static void anim_finish(struct ts_server *server) {
	if (!server->ws_anim.active) {
		return;
	}
	server->ws_anim.active = false;
	wl_event_source_timer_update(server->ws_anim.timer, 0);
	if (server->ws_anim.output) {
		arrange_output(server->ws_anim.output);
	}
	server->ws_anim.output = NULL;
	cursor_rebase(server);
}

static int anim_tick(void *data) {
	struct ts_server *server = data;
	if (!server->ws_anim.active) {
		return 0;
	}
	double progress = (now_ms() - server->ws_anim.start_ms) / (double)WS_ANIM_MS;
	if (progress >= 1.0) {
		anim_finish(server);
		return 0;
	}
	anim_place(server, progress);
	wl_event_source_timer_update(server->ws_anim.timer, 16);
	return 0;
}

void workspace_switch_output(struct ts_output *output, int workspace) {
	if (output == NULL || workspace < 1 || workspace > TS_WORKSPACES ||
			output->workspace == workspace) {
		return;
	}
	struct ts_server *server = output->server;
	anim_finish(server);
	int from = output->workspace;
	output->workspace = workspace;
	arrange_output(output);

	if (server->config.animations && !server->locked) {
		server->ws_anim.active = true;
		server->ws_anim.output = output;
		server->ws_anim.from = from;
		server->ws_anim.to = workspace;
		server->ws_anim.dir = workspace > from ? 1 : -1;
		server->ws_anim.start_ms = now_ms();
		anim_place(server, 0.0);
		wl_event_source_timer_update(server->ws_anim.timer, 16);
	}

	/* The keyboard stays on this screen, even if its new workspace is empty
	 * (this also sends the new state). */
	focus_output(server, output);
	cursor_rebase(server);
}

/* On the screen in use. */
void workspace_switch(struct ts_server *server, int workspace) {
	workspace_switch_output(output_focused(server), workspace);
}

/* Next or previous workspace, going at most one empty workspace past the
 * last one in use. */
void workspace_step_output(struct ts_output *output, int direction) {
	if (output == NULL) {
		return;
	}
	struct ts_server *server = output->server;
	int current = output->workspace;
	int last_used = 0;
	struct ts_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->mapped && view->output == output && view->workspace > last_used) {
			last_used = view->workspace;
		}
	}
	int limit = MAX(last_used + 1, current);
	limit = MIN(limit, TS_WORKSPACES);
	int target = current + direction;
	if (target >= 1 && target <= limit) {
		workspace_switch_output(output, target);
	}
}

/* On the screen in use. */
void workspace_step(struct ts_server *server, int direction) {
	workspace_step_output(output_focused(server), direction);
}

void layout_output_destroyed(struct ts_output *output) {
	struct ts_server *server = output->server;
	if (server->ws_anim.output == output) {
		server->ws_anim.active = false;
		server->ws_anim.output = NULL;
		wl_event_source_timer_update(server->ws_anim.timer, 0);
	}
}

void layout_init(struct ts_server *server) {
	server->ws_anim.timer = wl_event_loop_add_timer(server->loop, anim_tick, server);
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
	if (output == NULL || layout_count_tiled(output) < 2) {
		return false;
	}
	struct ts_view *view;
	wl_list_for_each(view, &output->server->views, link) {
		if (view->output == output && view_visible(view) &&
				(view->fullscreen || view->maximized)) {
			return false;
		}
	}
	int g = output_gaps(output);
	struct wlr_box area = tile_area(output);
	struct wlr_box first = tile_slot(area, g, output->master_ratio[output->workspace], 2, 0);
	int left = first.x + first.width;
	int pad = g < 8 ? 4 : 0;
	return lx >= left - pad && lx <= left + g + pad &&
		ly >= area.y && ly <= area.y + area.height;
}

void layout_set_ratio_from_cursor(struct ts_output *output, double lx) {
	int g = output_gaps(output);
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
