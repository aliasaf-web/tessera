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
	/* The slide between workspaces: a window is drawn where its layout put it
	 * plus where the slide has carried it. A client can commit at any moment
	 * while that runs, and it used to be drawn at the place it would end up -
	 * the whole window jumping home for a frame, which is what "the screen
	 * flickers when I change workspace" was. */
	const int widths[] = { 801, 1366 };
	for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
		int width = widths[w];
		for (int dir = -1; dir <= 1; dir += 2) {
			/* Neither end of the slide: the window's own place, whatever
			 * the direction and however far the slide has got. */
			for (double eased = 0.0; eased <= 1.0; eased += 0.5) {
				if (slide_offset(dir, width, eased, false, false) != 0) {
					fail("a window outside the slide stays where it is", width, dir);
				}
			}
			/* The ends of the slide are the window's own place too, and
			 * this is the check that matters: without it the window lands
			 * anywhere and the animation is only a guess. */
			if (slide_offset(dir, width, 0.0, true, false) != 0 ||
					slide_offset(dir, width, 1.0, true, false) != -dir * width) {
				fail("the window being left starts home and ends off the screen", width, dir);
			}
			if (slide_offset(dir, width, 0.0, false, true) != dir * width ||
					slide_offset(dir, width, 1.0, false, true) != 0) {
				fail("the window arriving starts off the screen and ends home", width, dir);
			}
			/* Halfway, the two are on opposite sides of the screen and
			 * neither is nearer the edge than half of it. */
			int out = slide_offset(dir, width, 0.5, true, false);
			int in = slide_offset(dir, width, 0.5, false, true);
			if (out != -in || out * dir > 0 || out == 0) {
				fail("halfway across, the two are half a screen apart", width, dir);
			}
		}
	}

	if (failures == 0) {
		printf("all tilings correct\n");
	}
	return failures ? 1 : 0;
}
