/* The tiling maths: windows fill the area exactly, with gaps between them
 * and no overlaps, for any number of windows and divider position. */
#include <stdio.h>
#include "../src/tile_math.h"

static int failures;

static void fail(const char *what, int n, int i) {
	printf("FAIL: %s (n=%d, i=%d)\n", what, n, i);
	failures++;
}

int main(void) {
	const struct wlr_box areas[] = { { 10, 20, 1346, 690 }, { 0, 0, 801, 599 }, { 5, 5, 160, 100 } };
	const double ratios[] = { 0.2, 0.55, 0.8 };
	for (size_t a = 0; a < sizeof(areas) / sizeof(areas[0]); a++) {
		for (size_t r = 0; r < sizeof(ratios) / sizeof(ratios[0]); r++) {
			for (int gap = 0; gap <= 12; gap += 6) {
				for (int n = 1; n <= 7; n++) {
					struct wlr_box area = areas[a];
					struct wlr_box first = tile_slot(area, gap, ratios[r], n, 0);
					if (n == 1) {
						if (first.x != area.x || first.y != area.y ||
								first.width != area.width || first.height != area.height) {
							fail("one window fills the area", n, 0);
						}
						continue;
					}
					if (first.x != area.x || first.y != area.y || first.height != area.height) {
						fail("the first window fills the left column", n, 0);
					}
					int expected_y = area.y;
					for (int i = 1; i < n; i++) {
						struct wlr_box b = tile_slot(area, gap, ratios[r], n, i);
						if (b.x != first.x + first.width + gap ||
								b.x + b.width != area.x + area.width) {
							fail("the right column starts after the gap and ends at the edge", n, i);
						}
						if (b.y != expected_y || b.height <= 0) {
							fail("windows follow each other with a gap", n, i);
						}
						expected_y = b.y + b.height + gap;
						if (i == n - 1 && b.y + b.height != area.y + area.height) {
							fail("the last window reaches the bottom", n, i);
						}
					}
				}
			}
		}
	}
	if (failures == 0) {
		printf("all tilings correct\n");
	}
	return failures ? 1 : 0;
}
