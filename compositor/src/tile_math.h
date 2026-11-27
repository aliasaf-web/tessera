#ifndef TESSERA_TILE_MATH_H
#define TESSERA_TILE_MATH_H

#include <stdbool.h>
#include <wlr/util/box.h>

/* Where tiled window i of n goes in area: the first fills the left column
 * (ratio of the width), the rest share the right column, top to bottom,
 * with gap pixels between them. */
static inline struct wlr_box tile_slot(struct wlr_box area, int gap, double ratio, int n, int i) {
	if (n <= 1) {
		return area;
	}
	int main_width = (int)((area.width - gap) * ratio);
	if (i == 0) {
		return (struct wlr_box){ area.x, area.y, main_width, area.height };
	}
	int stack_n = n - 1;
	int total = area.height - (stack_n - 1) * gap;
	int idx = i - 1;
	int y = area.y + idx * (total / stack_n + gap);
	int height = idx == stack_n - 1 ? area.y + area.height - y : total / stack_n;
	return (struct wlr_box){ area.x + main_width + gap, y, area.width - main_width - gap, height };
}

/* How far the slide between two workspaces has carried a window, in pixels:
 * `eased` of the way across a screen `width` wide, in the direction the slide
 * travels. A window on the workspace being left moves out of the screen, one on
 * the workspace arrived at moves into it, and at either end of the slide the
 * answer is exactly 0 - the window's own place. That is what lets a window that
 * commits mid-slide be drawn where the slide has it rather than where it will
 * end up. */
static inline int slide_offset(int dir, int width, double eased, bool from, bool to) {
	if (from) {
		return (int)(-dir * width * eased);
	}
	if (to) {
		return (int)(dir * width * (1.0 - eased));
	}
	return 0;
}

#endif
