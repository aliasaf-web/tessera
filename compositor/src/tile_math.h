#ifndef TESSERA_TILE_MATH_H
#define TESSERA_TILE_MATH_H

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

#endif
