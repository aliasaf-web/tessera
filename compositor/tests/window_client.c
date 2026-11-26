/* A window, a panel, a menu, and a hand on the screens.
 *
 * The desktop's own tests (tests/test_compositor.py) can ask it questions over
 * its control socket, but a question is worth little while nothing is in the
 * desktop to answer it: no window to tile, no bar to make room for, no menu to
 * keep on the screen, nothing on the receiving end of the interfaces the lock
 * screen holds back. This is that something, so that a test can check what the
 * compositor does rather than what it says it does. Four modes:
 *
 *   --globals         list the interfaces this desktop advertises, and exit
 *   --window          map one window and say what size it is asked for
 *   --panel             map one layer surface - a bar - and say the size it is
 *   --zone N              given, reserving N pixels of the screen while it is up
 *   --drive-outputs     hold the output manager, and stay up until told
 *
 * and every one of them reads commands on its standard input, a line at a time:
 *
 *   stop              end, which is also what the end of the pipe means
 *   size              say again the size the window was last asked for
 *   popup             open a menu below the window, and say where it was put
 *   heads             say where each screen is, as this client is told
 *   test NAME X Y     ask whether that move would be accepted, and stop there
 *   apply NAME X Y    ask for one screen to be moved
 *   allmodes W H      give every screen the same mode and no position, which is
 *                     the request that once piled two screens in one corner
 *
 * Everything it prints is one line starting with a word, flushed as it is
 * written, so a test never has to tell a program that is thinking from one that
 * is stuck. It is built by the same meson project as the window manager, next to
 * it, and installed nowhere.
 *
 * Two habits here were paid for on the CI machine, the only one that can compile
 * this file, and both are about the same trap: what a protocol object's events
 * are is the generated header's business, not this file's. Listeners use
 * designated initialisers, because a struct in the wrong order or with one too
 * many is a compile error nothing here can see first. And every listener covers
 * *all* the events of the version it bound, filling in do-nothing functions for
 * the ones it does not care about - libwayland checks that on the spot and aborts
 * the client, which is a worse failure than a warning, because it says only
 * which opcode was left out. Objects the compositor may take back are left alone
 * rather than destroyed: this client is going to exit anyway, and a request sent
 * to an id the compositor has already released is a protocol error blamed on
 * us. */

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"

#define WIDTH 200
#define HEIGHT 120
#define PANEL_H 40       /* what a bar asks to be, in pixels of screen */
#define POPUP_W 120      /* a menu is sized by the client that opens it */
#define POPUP_H 100
#define POPUP_GAP 60     /* how far below the window's edge to ask for it */

/* One screen, as the output manager describes it. Only the name and the place
 * are kept, which is all the commands and the lines printed about them need. */
struct head {
	char name[96];
	struct zwlr_output_head_v1 *head;
	int x, y;
	int live;
};

struct client {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	struct wl_buffer *buffer;        /* what the window last handed over */
	struct wl_buffer *panel_buffer;
	struct wl_buffer *popup_buffer;
	int popup_shown;
	uint32_t argb;
	int want_argb;
	int want_xrgb;
	int width;
	int height;
	uint32_t configure;     /* the serial of a configure not yet answered */
	int resize;             /* the size changed, and the new buffer is owed */
	int shown;              /* a buffer has been committed once */
	int running;
	int failed;
	int idle_ms;

	/* A menu, and where the desktop put it. */
	struct xdg_surface *popup_xdg;
	struct wl_surface *popup_wl;
	struct xdg_popup *popup;

	/* --panel: one layer surface, and whether the desktop has sized it yet. */
	int panel;
	int panel_zone;
	unsigned panel_anchor;
	int panel_shown;
	struct wl_surface *panel_surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct zwlr_layer_shell_v1 *layer_shell;

	/* --drive-outputs: the screens, as this client is told about them. */
	int drive;
	struct zwlr_output_manager_v1 *output_manager;
	int manager_serial;      /* the serial a configuration has to be made with */
	int manager_ready;
	struct zwlr_output_configuration_v1 *config; /* the one awaiting a verdict */
	char config_verb[16];    /* what to print when that verdict arrives */
	struct head heads[16];
	int n_heads;
	char cmd[192];
	size_t cmd_len;
};

/* One line, out now: a test should not have to wonder whether we are thinking.
 * The printf attribute is what lets the compiler check the lines below. */
static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *format, ...) {
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	fflush(stdout);
}

/* ------------------------------------------------------------------ shared */

/* A buffer of `width` x `height`, painted one flat colour, attached to `surface`
 * and committed; `label` is printed once, when the first one lands - "mapped"
 * for a window, something else for a panel or a menu, so a test waits on the
 * word that matches the thing it asked for. The colour is nothing; that the
 * whole rectangle arrives is the point. */
static int paint(struct client *client, struct wl_surface *surface,
		struct wl_buffer **slot, const char *label, int width, int height, int *shown) {
	size_t stride = (size_t)width * 4;
	size_t size = stride * (size_t)height;
	int fd = memfd_create("tessera-test-window", MFD_CLOEXEC);
	if (fd < 0) {
		say("error buffer: %s\n", strerror(errno));
		return -1;
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		say("error buffer: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		say("error buffer: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	for (size_t i = 0; i < size / 4; i++) {
		pixels[i] = 0xFF3070C0;   /* opaque blue; the alpha byte is ignored on XRGB */
	}
	munmap(pixels, size);

	struct wl_shm_pool *pool = wl_shm_create_pool(client->shm, fd, (int32_t)size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
			(int32_t)stride, client->argb);
	wl_shm_pool_destroy(pool);
	close(fd);
	if (buffer == NULL) {
		say("error the desktop would not take the buffer\n");
		return -1;
	}
	/* The buffer handed over last is left alone rather than destroyed here: a
	 * client may only let go of one the compositor has released, and a surface
	 * that is resized a few times can be allowed to keep a few. */
	*slot = buffer;
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, width, height);
	wl_surface_commit(surface);
	if (!*shown) {
		*shown = 1;
		say("%s %dx%d\n", label, width, height);
	}
	return 0;
}

/* A window paints exactly what the desktop asked for, and no more. */
static int paint_window(struct client *client) {
	return paint(client, client->surface, &client->buffer, "mapped", client->width,
		client->height, &client->shown);
}

/* ------------------------------------------------------------------ popup */

/* xdg-shell asks for these three even though this client has no use for them:
 * at the version bound above, a listener with a hole in it is an abort, not a
 * warning - see the note by `head_listener`. `xdg_surface` is not one of them:
 * its `destroy` is a request the client sends, not an event, and the one event
 * it does have is the configure this file already answers. */
static void toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel,
		int32_t width, int32_t height) {
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel,
		struct wl_array *capabilities) {
}

static void popup_repositioned(void *data, struct xdg_popup *popup, uint32_t token) {
	say("menu repositioned\n");
}

static void popup_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
	struct client *client = data;
	xdg_surface_ack_configure(surface, serial);
	/* The same rule as a window's, and for the same reason: the first commit of
	 * an xdg_surface says "ask me" and carries nothing, and a buffer in it is an
	 * error the desktop answers by ending the connection - `xdg_surface has never
	 * been configured`, which is what this order was paid for. What a menu is
	 * painted with is the client's own business; where the desktop put it is the
	 * answer this client exists to print, and that comes on the popup itself. */
	if (!client->popup_shown) {
		if (paint(client, client->popup_wl, &client->popup_buffer, "menu painted",
				POPUP_W, POPUP_H, &client->popup_shown) < 0) {
			client->failed = 1;
			client->running = 0;
		}
	}
}

static void popup_configure(void *data, struct xdg_popup *popup, int32_t x, int32_t y,
		int32_t width, int32_t height) {
	/* The position is relative to the window's own surface, which is what makes
	 * it readable: a menu asked for under the bottom edge of a window that sits
	 * near it comes back with a negative y, because the desktop lifted it. */
	say("popup %d %d %dx%d\n", x, y, width, height);
}

static const struct xdg_surface_listener popup_surface_listener = {
	.configure = popup_surface_configure,
};

/* The compositor is done with the menu - the client dismissed it, or it was
 * asked for a parent that went away. Named `menu` rather than `popup` so that a
 * test waiting on the placement line cannot mistake this for one. */
static void popup_done(void *data, struct xdg_popup *popup) {
	say("menu done\n");
}

static const struct xdg_popup_listener popup_listener = {
	.configure = popup_configure,
	.popup_done = popup_done,
	.repositioned = popup_repositioned,
};

/* Open the menu below the window's bottom-left corner, a screen's height down
 * from it: far enough out that only a desktop that pays attention to the screen
 * edge can put it back where it belongs. */
static void open_popup(struct client *client) {
	struct xdg_positioner *positioner = xdg_wm_base_create_positioner(client->wm_base);
	xdg_positioner_set_size(positioner, POPUP_W, POPUP_H);
	/* The whole window is the anchor rectangle and the menu hangs off its bottom
	 * edge, 60 pixels below: which is the everyday shape of a context menu that
	 * opens under a window at the bottom of the screen, and the desktop needs the
	 * rectangle to be told at all - xdg-shell answers a positioner without one
	 * with a protocol error rather than a menu. From here on, only a desktop that
	 * looks at the screen edge has a reason to send the number back smaller. */
	xdg_positioner_set_anchor_rect(positioner, 0, 0, client->width, client->height);
	xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_BOTTOM);
	xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_TOP);
	xdg_positioner_set_offset(positioner, 0, POPUP_GAP);
	/* A menu is a surface of its own, with its own xdg_surface - the popup is
	 * what says whose it is. Unlike a window, it is sized by the client that
	 * opens it; like one, its first commit carries nothing, and the buffer comes
	 * after the answer is acknowledged - which is what `popup_surface_configure`
	 * below does. */
	client->popup_wl = wl_compositor_create_surface(client->compositor);
	client->popup_xdg = xdg_wm_base_get_xdg_surface(client->wm_base, client->popup_wl);
	client->popup = xdg_surface_get_popup(client->popup_xdg, client->xdg_surface, positioner);
	xdg_popup_add_listener(client->popup, &popup_listener, client);
	xdg_surface_add_listener(client->popup_xdg, &popup_surface_listener, client);
	wl_surface_commit(client->popup_wl);
}

/* ------------------------------------------------------------------ panel */

static void panel_configure(void *data, struct zwlr_layer_surface_v1 *surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct client *client = data;
	/* The desktop decides the size of a panel on the axes it is anchored to, so
	 * this is the answer to "how much of the top of my screen is mine": the
	 * width it is handed is the screen's, and the height is the one asked for. */
	zwlr_layer_surface_v1_ack_configure(surface, serial);
	say("panel %dx%d\n", width, height);
	/* A panel is sized by the desktop, not merely asked: whatever is painted on
	 * it has to be the rectangle that was granted, or the compositor rejects the
	 * buffer and the bar stays empty. */
	if (paint(client, client->panel_surface, &client->panel_buffer, "panel painted",
			(int)width, (int)height, &client->panel_shown) < 0) {
		client->failed = 1;
		client->running = 0;
	}
}

static void panel_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
	struct client *client = data;
	say("panel closed\n");
	client->running = 0;
}

static const struct zwlr_layer_surface_v1_listener panel_listener = {
	.configure = panel_configure,
	.closed = panel_closed,
};

static void open_panel(struct client *client) {
	client->panel_surface = wl_compositor_create_surface(client->compositor);
	client->layer_surface = zwlr_layer_shell_v1_get_layer_surface(client->layer_shell,
		client->panel_surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "tessera-test-panel");
	zwlr_layer_surface_v1_add_listener(client->layer_surface, &panel_listener, client);
	/* Across the whole top edge, which is the shape of a bar and the only anchor
	 * pair a compositor can hand a full width to. It is not a choice this client
	 * offers: a side or bottom bar would answer the same question, and one place
	 * to ask it is enough for a test. */
	zwlr_layer_surface_v1_set_anchor(client->layer_surface, client->panel_anchor);
	/* The width is left at zero on purpose: anchored left and right, a bar is as
	 * wide as the desktop says, which is the half of this a test can check. */
	zwlr_layer_surface_v1_set_size(client->layer_surface, 0, PANEL_H);
	zwlr_layer_surface_v1_set_exclusive_zone(client->layer_surface, client->panel_zone);
	zwlr_layer_surface_v1_set_margin(client->layer_surface, 0, 0, 0, 0);
	wl_surface_commit(client->panel_surface);
}

/* ------------------------------------------------------- the output manager */

static void head_name(void *data, struct zwlr_output_head_v1 *head, const char *name) {
	struct head *record = data;
	snprintf(record->name, sizeof(record->name), "%s", name);
}

static void head_position(void *data, struct zwlr_output_head_v1 *head, int32_t x, int32_t y) {
	struct head *record = data;
	record->x = x;
	record->y = y;
}

/* A head that changes is retired and re-advertised, so this says only that the
 * object a configuration would have to be made with is gone. The record itself
 * stays where it is: a compositor's listener points at it, and moving it about
 * would leave that pointing at the wrong screen. It is marked dead instead, and
 * the next head to appear takes the slot. */
static void head_finished(void *data, struct zwlr_output_head_v1 *head) {
	struct client *client = data;
	for (int i = 0; i < client->n_heads; i++) {
		if (client->heads[i].head == head) {
			client->heads[i].live = 0;
			client->heads[i].head = NULL;
			break;
		}
	}
}

/* libwayland will not let a client leave a hole in a listener struct: for every
 * event the version it bound can be sent, there has to be a function there, and
 * a missing one is `listener function for opcode N of ... is NULL` and an
 * abort - paid for here the first time a head arrived, with three lines of
 * reason and no window in the desktop to show for it. So the eight events below
 * that this client has no use for are answered by things that do nothing, which
 * is also what they are for: the name and the place are the whole of what a test
 * here needs, and the compositor is allowed to say the rest whenever it likes. */
static void head_nothing_string(void *data, struct zwlr_output_head_v1 *head,
		const char *text) {
}

static void head_nothing_size(void *data, struct zwlr_output_head_v1 *head, int32_t width,
		int32_t height) {
}

static void head_nothing_number(void *data, struct zwlr_output_head_v1 *head, int32_t number) {
}

static void head_nothing_mode(void *data, struct zwlr_output_head_v1 *head,
		struct zwlr_output_mode_v1 *mode) {
}

static void head_nothing_fixed(void *data, struct zwlr_output_head_v1 *head, wl_fixed_t scale) {
}

static const struct zwlr_output_head_v1_listener head_listener = {
	.name = head_name,
	.description = head_nothing_string,
	.physical_size = head_nothing_size,
	.mode = head_nothing_mode,
	.enabled = head_nothing_number,
	.current_mode = head_nothing_mode,
	.position = head_position,
	.transform = head_nothing_number,
	.scale = head_nothing_fixed,
	.finished = head_finished,
};

static void manager_head(void *data, struct zwlr_output_manager_v1 *manager,
		struct zwlr_output_head_v1 *head) {
	struct client *client = data;
	struct head *record = NULL;
	for (int i = 0; i < client->n_heads; i++) {
		if (!client->heads[i].live) {
			record = &client->heads[i];
			break;
		}
	}
	if (record == NULL) {
		if (client->n_heads >= (int)(sizeof(client->heads) / sizeof(client->heads[0]))) {
			say("error more screens than this client can hold\n");
			return;
		}
		record = &client->heads[client->n_heads++];
		record->name[0] = '\0';
	}
	record->head = head;
	record->x = record->y = 0;
	record->live = 1;
	zwlr_output_head_v1_add_listener(head, &head_listener, record);
}

static void manager_done(void *data, struct zwlr_output_manager_v1 *manager, uint32_t serial) {
	struct client *client = data;
	client->manager_serial = (int)serial;
	if (!client->manager_ready) {
		say("outputs ready %d\n", client->n_heads);
	}
	client->manager_ready = 1;
	for (int i = 0; i < client->n_heads; i++) {
		if (client->heads[i].live) {
			say("head %s at %d,%d\n", client->heads[i].name, client->heads[i].x,
				client->heads[i].y);
		}
	}
}

static void manager_finished(void *data, struct zwlr_output_manager_v1 *manager) {
	/* The manager is going away: the desktop is closing down. */
	((struct client *)data)->running = 0;
}

static const struct zwlr_output_manager_v1_listener manager_listener = {
	.head = manager_head,
	.done = manager_done,
	.finished = manager_finished,
};

static void config_verdict(struct client *client, const char *verdict) {
	say("%s %s\n", client->config_verb, verdict);
	/* Not destroyed: the compositor has already taken the object away by the
	 * time it answers, and a request on an id it released is a protocol error
	 * in the desktop rather than news about the screens. */
	client->config = NULL;
	client->config_verb[0] = '\0';
}

static void config_succeeded(void *data, struct zwlr_output_configuration_v1 *config) {
	config_verdict(data, "ok");
}

static void config_failed(void *data, struct zwlr_output_configuration_v1 *config) {
	config_verdict(data, "failed");
}

static void config_cancelled(void *data, struct zwlr_output_configuration_v1 *config) {
	config_verdict(data, "cancelled");
}

static const struct zwlr_output_configuration_v1_listener config_listener = {
	.succeeded = config_succeeded,
	.failed = config_failed,
	.cancelled = config_cancelled,
};

static struct head *find_head(struct client *client, const char *name) {
	for (int i = 0; i < client->n_heads; i++) {
		if (client->heads[i].live && strcmp(client->heads[i].name, name) == 0) {
			return &client->heads[i];
		}
	}
	return NULL;
}

/* Starts a configuration and puts one live head in it, or says why not. Every
 * command goes through here so that the answers come back in the same shape. */
static struct zwlr_output_configuration_head_v1 *begin(struct client *client, const char *verb,
		struct head *head) {
	if (client->config != NULL) {
		say("%s busy\n", verb);
		return NULL;
	}
	if (client->output_manager == NULL || !client->manager_ready) {
		/* No manager, or no list of screens yet: while the screen is locked the
		 * interface is not offered at all, so "no" is the honest answer rather
		 * than a hang. */
		say("%s unavailable\n", verb);
		return NULL;
	}
	client->config = zwlr_output_manager_v1_create_configuration(client->output_manager,
		(uint32_t)client->manager_serial);
	if (client->config == NULL) {
		say("%s refused\n", verb);
		return NULL;
	}
	zwlr_output_configuration_v1_add_listener(client->config, &config_listener, client);
	snprintf(client->config_verb, sizeof(client->config_verb), "%s", verb);
	return zwlr_output_configuration_v1_enable_head(client->config, head->head);
}

/* "NAME X Y", applied or only tried out: the one request a screen can be moved
 * by from outside, which is what the settings window does through this same
 * protocol - `test` first, and `apply` only if the answer was yes. */
static void command_place(struct client *client, const char *verb, char *args, int x, int y) {
	struct head *head = find_head(client, args);
	if (head == NULL) {
		say("%s no-such-head\n", verb);
		return;
	}
	struct zwlr_output_configuration_head_v1 *cfg_head = begin(client, verb, head);
	if (cfg_head == NULL) {
		return;   /* busy, refused, or not listening; begin() said which */
	}
	zwlr_output_configuration_head_v1_set_position(cfg_head, x, y);
	if (strcmp(verb, "test") == 0) {
		zwlr_output_configuration_v1_test(client->config);
	} else {
		zwlr_output_configuration_v1_apply(client->config);
	}
}

/* "W H": every screen given a mode, and the position a client that never
 * mentioned one leaves the protocol to fill in: 0,0. That is the request that
 * used to pile the screens on top of each other in the corner, because the
 * desktop has to guess whether a corner was meant or merely left blank - so the
 * client sends it on purpose, in the same shape wlr-randr does, and the test
 * asks where the screens ended up. */
static void command_allmodes(struct client *client, int width, int height) {
	int live = 0;
	for (int i = 0; i < client->n_heads; i++) {
		live += client->heads[i].live;
	}
	if (live == 0) {
		say("allmodes no-heads\n");
		return;
	}
	if (client->config != NULL) {
		say("allmodes busy\n");
		return;
	}
	if (!client->manager_ready || client->output_manager == NULL) {
		say("allmodes unavailable\n");
		return;
	}
	client->config = zwlr_output_manager_v1_create_configuration(client->output_manager,
		(uint32_t)client->manager_serial);
	if (client->config == NULL) {
		say("allmodes refused\n");
		return;
	}
	zwlr_output_configuration_v1_add_listener(client->config, &config_listener, client);
	snprintf(client->config_verb, sizeof(client->config_verb), "allmodes");
	for (int i = 0; i < client->n_heads; i++) {
		if (!client->heads[i].live) {
			continue;
		}
		struct zwlr_output_configuration_head_v1 *cfg_head =
			zwlr_output_configuration_v1_enable_head(client->config, client->heads[i].head);
		if (cfg_head == NULL) {
			/* The compositor will not let this one be configured. Saying so is
			 * the point of the client; asking it anyway is a crash in a file the
			 * test is not reading. */
			say("allmodes no-head %s\n", client->heads[i].name);
			return;
		}
		zwlr_output_configuration_head_v1_set_custom_mode(cfg_head, width, height, 60000);
		zwlr_output_configuration_head_v1_set_position(cfg_head, 0, 0);
	}
	zwlr_output_configuration_v1_apply(client->config);
}

static void run_command(struct client *client, char *line) {
	char verb[16];
	if (sscanf(line, "%15s", verb) != 1) {
		return;
	}
	char *rest = line + strlen(verb);
	while (*rest == ' ' || *rest == '\t') {
		rest++;
	}
	if (strcmp(verb, "stop") == 0 || strcmp(verb, "quit") == 0) {
		say("stop\n");
		client->running = 0;
	} else if (strcmp(verb, "popup") == 0) {
		if (client->xdg_surface == NULL) {
			say("popup no-window\n");
		} else if (client->popup != NULL) {
			say("popup busy\n");
		} else {
			open_popup(client);
		}
	} else if (strcmp(verb, "size") == 0) {
		/* What the desktop last asked this window to be, said again on request:
		 * a test that wants to know whether anything *changed* cannot tell that
		 * from an absent line, and can tell it from this one. */
		if (client->xdg_surface == NULL) {
			say("size no-window\n");
		} else {
			say("size %dx%d\n", client->width, client->height);
		}
	} else if (strcmp(verb, "heads") == 0) {
		int live = 0;
		for (int i = 0; i < client->n_heads; i++) {
			if (!client->heads[i].live) {
				continue;
			}
			live++;
			say("head %s at %d,%d\n", client->heads[i].name, client->heads[i].x,
				client->heads[i].y);
		}
		say("heads %d\n", live);
	} else if (strcmp(verb, "apply") == 0) {
		char name[96];
		int x = 0, y = 0;
		if (sscanf(rest, "%95s %d %d", name, &x, &y) != 3) {
			say("apply error usage\n");
			return;
		}
		command_place(client, "apply", name, x, y);
	} else if (strcmp(verb, "test") == 0) {
		char name[96];
		int x = 0, y = 0;
		if (sscanf(rest, "%95s %d %d", name, &x, &y) != 3) {
			say("test error usage\n");
			return;
		}
		command_place(client, "test", name, x, y);
	} else if (strcmp(verb, "allmodes") == 0) {
		int width = 0, height = 0;
		if (sscanf(rest, "%d %d", &width, &height) != 2) {
			say("allmodes error usage\n");
			return;
		}
		command_allmodes(client, width, height);
	} else {
		say("error unknown command %s\n", verb);
	}
}

/* The lines the test writes to us. `stop`, or the end of the pipe, ends the
 * client - which is what it did before there were commands, and is still the
 * common case: a test that only wants a window up writes nothing and closes it.
 * Returns 1 when the caller should go round again, 0 when it should not. */
static int read_commands(struct client *client) {
	char buf[128];
	ssize_t got = read(STDIN_FILENO, buf, sizeof(buf));
	if (got <= 0) {
		say("stop\n");
		client->running = 0;
		return 0;
	}
	for (ssize_t i = 0; i < got; i++) {
		if (buf[i] != '\n') {
			if (client->cmd_len + 1 < sizeof(client->cmd)) {
				client->cmd[client->cmd_len++] = buf[i];
			}
			continue;
		}
		client->cmd[client->cmd_len] = '\0';
		client->cmd_len = 0;
		run_command(client, client->cmd);
		if (!client->running) {
			return 0;
		}
	}
	return 1;
}

/* ---------------------------------------------------------------- registry */

static void shm_format(void *data, struct wl_shm *shm, uint32_t format) {
	struct client *client = data;
	if (format == WL_SHM_FORMAT_ARGB8888) {
		client->want_argb = 1;
	} else if (format == WL_SHM_FORMAT_XRGB8888) {
		client->want_xrgb = 1;
	}
}

static const struct wl_shm_listener shm_listener = {
	.format = shm_format,
};

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	/* A desktop that wonders whether a window is still there gets an answer,
	 * or it takes the window away from a test that is still reading it. */
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	struct client *client = data;
	say("global %s %u\n", interface, version);
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		client->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		client->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
		wl_shm_add_listener(client->shm, &shm_listener, client);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		/* The version the desktop offers, capped at the one this file was
		 * written against. Version 1 would be the smallest thing that works for
		 * a plain window, but a menu needs an anchor rectangle and that is a
		 * version 4 request - and a client that asks for the current version is
		 * also what the desktop is really being used with. */
		client->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
			version < 6 ? version : 6);
		xdg_wm_base_add_listener(client->wm_base, &wm_base_listener, client);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		client->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface,
			version < 3 ? version : 3);
	} else if (strcmp(interface, zwlr_output_manager_v1_interface.name) == 0) {
		/* Held back while the screen is locked, so this can be NULL - which the
		 * commands answer to themselves, rather than the client giving up on a
		 * desktop that is only shut for the moment.
		 *
		 * Version 1, deliberately, although the desktop offers 4: the requests
		 * this client makes (a configuration, a head in it, a mode, a place)
		 * are all of the first version, and a lower version is the one place a
		 * client can say which events it is willing to be asked about. The head
		 * listener below covers the ten events of version 1; versions 2 and 4
		 * add four more, and a client that bound 4 would have to answer those
		 * too, in code that could never be anything but empty. */
		client->output_manager = wl_registry_bind(registry, name,
			&zwlr_output_manager_v1_interface, version < 1 ? version : 1);
		if (client->output_manager != NULL) {
			zwlr_output_manager_v1_add_listener(client->output_manager, &manager_listener,
				client);
		}
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
	/* The desktop takes interfaces away while the screen is locked and hands
	 * them back afterwards; a test that cares asks again rather than keeping a
	 * list from before. */
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* --------------------------------------------------------------- xdg-shell */

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
	struct client *client = data;
	/* Nothing is drawn before this answer, which is the whole of xdg-shell's
	 * rule about the first configure as well as about later ones. It is also
	 * the only event xdg_surface has: the listeners for the ones with more of
	 * them are one or two lines longer, and the compiler is the only thing that
	 * tells you so. */
	xdg_surface_ack_configure(surface, serial);
	client->configure = serial;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width,
		int32_t height, struct wl_array *states) {
	struct client *client = data;
	/* `states` and the toplevel itself go unread: this client has nothing to
	 * look maximised or fullscreen, only a size to agree to. The answer to a
	 * configure goes on the xdg_surface, not here - acking on the toplevel is
	 * a request that only exists from xdg-shell version 3, and this client
	 * binds the version the desktop offers, whatever that is. */
	if (width <= 0) {
		width = WIDTH;
	}
	if (height <= 0) {
		height = HEIGHT;
	}
	/* Said whether or not it is news: a test reads these lines to know what the
	 * desktop asked for, and one that agrees with the size the client came in
	 * with is still the answer to what a fullscreen did to it. */
	say("configure %dx%d\n", width, height);
	if (width == client->width && height == client->height) {
		return;
	}
	client->width = width;
	client->height = height;
	if (client->shown) {
		client->resize = 1;
	}
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
	struct client *client = data;
	say("closed\n");
	client->running = 0;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_configure_bounds,
	.wm_capabilities = toplevel_wm_capabilities,
};

/* ---------------------------------------------------------------- dispatch */

/* Reads until something happens, or the wait runs out, or the test writes to us.
 * Returns 1 to go on, 0 for a wait of our own that came to nothing, and -1 when
 * the desktop went away mid-conversation. */
static int pump(struct client *client, int ms) {
	if (wl_display_prepare_read(client->display) != 0) {
		if (wl_display_dispatch_pending(client->display) < 0) {
			return -1;
		}
		return 1;
	}
	if (wl_display_flush(client->display) < 0 && errno != EAGAIN) {
		wl_display_cancel_read(client->display);
		return -1;
	}
	struct pollfd fds[] = {
		{ .fd = wl_display_get_fd(client->display), .events = POLLIN },
		{ .fd = STDIN_FILENO, .events = POLLIN },
	};
	if (poll(fds, 2, ms) <= 0) {
		wl_display_cancel_read(client->display);
		return 0;
	}
	if (fds[1].revents) {
		wl_display_cancel_read(client->display);
		return read_commands(client);
	}
	if (wl_display_read_events(client->display) < 0) {
		return -1;
	}
	return wl_display_dispatch_pending(client->display) < 0 ? -1 : 1;
}

static void usage(const char *name) {
	printf("Usage:\n"
		"  %s --globals              list the interfaces this desktop offers, and exit\n"
		"  %s --window [options]     map one window and stay until told to stop\n"
		"  %s --panel [options]      map one layer surface (a bar) and stay up\n"
		"  %s --drive-outputs        take orders about the screens on stdin\n"
		"  --title TEXT, --app-id NAME, --idle-ms N (default 30000),\n"
		"  --zone N (with --panel), --drive-outputs with --window to watch a\n"
		"  screen change from the inside.\n"
		"Commands on stdin: stop, size (say the window's size again), popup\n"
		"(a menu below the window), and the screen commands above.\n",
		name, name, name, name);
}

int main(int argc, char **argv) {
	struct client client = { 0 };
	client.width = WIDTH;
	client.height = HEIGHT;
	client.running = 1;
	client.idle_ms = 30000;
	const char *title = "Tessera test window";
	const char *app_id = "tessera-test-window";
	int globals_only = 0;
	int make_window = 0;
	/* Across the whole top edge: a bar is what the exclusive-zone rule is for,
	 * and it is the only shape this desktop hands a full width to. */
	client.panel_anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
		| ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--globals") == 0) {
			globals_only = 1;
		} else if (strcmp(argv[i], "--window") == 0) {
			make_window = 1;
		} else if (strcmp(argv[i], "--panel") == 0) {
			client.panel = 1;
		} else if (strcmp(argv[i], "--zone") == 0 && i + 1 < argc) {
			client.panel = 1;
			client.panel_zone = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--drive-outputs") == 0) {
			client.drive = 1;
		} else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
			title = argv[++i];
		} else if (strcmp(argv[i], "--app-id") == 0 && i + 1 < argc) {
			app_id = argv[++i];
		} else if (strcmp(argv[i], "--idle-ms") == 0 && i + 1 < argc) {
			client.idle_ms = atoi(argv[++i]);
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	client.display = wl_display_connect(NULL);
	if (client.display == NULL) {
		say("error there is no desktop to talk to (WAYLAND_DISPLAY is %s)\n",
				getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "not set");
		return 1;
	}
	client.registry = wl_display_get_registry(client.display);
	wl_registry_add_listener(client.registry, &registry_listener, &client);
	if (wl_display_roundtrip(client.display) < 0) {
		say("error the desktop did not answer the first question\n");
		return 1;
	}
	if (globals_only) {
		/* What the lock screen holds back shows up here as something absent,
		 * which is how it works: the bind is refused, so a program that starts
		 * now never hears of the interface at all. */
		wl_display_disconnect(client.display);
		return 0;
	}
	if (make_window && (client.compositor == NULL || client.shm == NULL
			|| client.wm_base == NULL)) {
		say("error this desktop has no compositor, no shared memory, or no xdg-shell\n");
		return 1;
	}
	if (client.panel && (client.compositor == NULL || client.layer_shell == NULL)) {
		say("error this desktop has no layer shell to hang a bar on\n");
		return 1;
	}
	if (client.drive && client.output_manager == NULL) {
		/* Not a failure of the desktop: the output manager is one of the things
		 * the lock screen holds back, so this is the line to print when the test
		 * asked for it at a moment it is not there. */
		say("drive unavailable\n");
		return 0;
	}
	if ((make_window || client.panel) && wl_display_roundtrip(client.display) < 0) {
		say("error the desktop did not say what its shared memory can hold\n");
		return 1;
	}
	if (client.want_argb) {
		client.argb = WL_SHM_FORMAT_ARGB8888;
	} else if (client.want_xrgb) {
		client.argb = WL_SHM_FORMAT_XRGB8888;
	} else if (make_window || client.panel) {
		say("error this desktop shares memory in no shape we can draw into\n");
		return 1;
	}

	if (make_window) {
		client.surface = wl_compositor_create_surface(client.compositor);
		client.xdg_surface = xdg_wm_base_get_xdg_surface(client.wm_base, client.surface);
		xdg_surface_add_listener(client.xdg_surface, &xdg_surface_listener, &client);
		client.toplevel = xdg_surface_get_toplevel(client.xdg_surface);
		xdg_toplevel_add_listener(client.toplevel, &toplevel_listener, &client);
		xdg_toplevel_set_title(client.toplevel, title);
		xdg_toplevel_set_app_id(client.toplevel, app_id);
		/* The first commit carries no buffer: xdg-shell wants the answer to the
		 * first configure first, and a window that skips that is told so, loudly. */
		wl_surface_commit(client.surface);
	}
	if (client.panel) {
		open_panel(&client);
	}

	int waited = 0;
	while (client.running) {
		if (client.configure) {
			client.configure = 0;
			if (paint_window(&client) < 0) {
				client.failed = 1;
				break;
			}
		}
		if (client.resize) {
			client.resize = 0;
			if (paint_window(&client) < 0) {
				client.failed = 1;
				break;
			}
		}
		int went = pump(&client, 200);
		if (went < 0) {
			say("error the desktop went away while the window was up\n");
			client.failed = 1;
			break;
		}
		/* A test waiting on `--drive-outputs` decides when it is done, so nobody
		 * is allowed to run out of patience here; every other mode stops itself
		 * rather than staying up for the length of the suite. */
		if (!client.drive) {
			waited += 200;
			if (waited >= client.idle_ms) {
				say("timeout\n");
				break;
			}
		}
	}

	if (client.toplevel) {
		xdg_toplevel_destroy(client.toplevel);
	}
	if (client.xdg_surface) {
		xdg_surface_destroy(client.xdg_surface);
	}
	if (client.surface) {
		wl_surface_destroy(client.surface);
	}
	wl_display_flush(client.display);
	wl_display_disconnect(client.display);
	return client.failed ? 1 : 0;
}
