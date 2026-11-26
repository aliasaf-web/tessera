/* A window, and a look at what the desktop is offering.
 *
 * The desktop's own tests (tests/test_compositor.py) can ask it questions over
 * its control socket, but until now there was nothing *in* the desktop to ask
 * about: no window to move or close, and nothing on the receiving end of the
 * interfaces the lock screen holds back. This is that client, so that a test can
 * check what the compositor does rather than what it says it does. Two dull
 * things:
 *
 *   --globals   list the interfaces this desktop advertises, and exit
 *   --window    map one window, say what size it is asked for, stay up
 *
 * Everything printed is one line starting with a word, flushed as it is written,
 * so a test never has to tell apart a program that is thinking from a program
 * that is stuck. Writing anything to its standard input ends it. It is built by
 * the same meson project as the window manager, next to it, and installed
 * nowhere.
 */

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

#define WIDTH 200
#define HEIGHT 120

struct client {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	struct wl_buffer *buffer;
	uint32_t argb;
	int want_argb;
	int want_xrgb;
	int width;
	int height;
	uint32_t configure;     /* the serial of a configure not yet answered */
	int resize;             /* the size changed, and the new buffer is owed */
	int shown;               /* a buffer has been committed once */
	int running;
	int failed;
	int idle_ms;
};

/* One line, out now: a test should not have to wonder whether we are thinking. */
static void say(const char *format, ...) {
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	fflush(stdout);
}

/* ------------------------------------------------------------------ shared */

/* A buffer of the size the compositor last asked for, painted one flat colour.
 * The colour is nothing; that the whole rectangle arrives is the point. */
static int paint(struct client *client) {
	size_t stride = (size_t)client->width * 4;
	size_t size = stride * (size_t)client->height;
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
	for (size_t i = 0; i < stride / 4 * (size_t)client->height; i++) {
		pixels[i] = 0xFF3070C0;   /* opaque blue; the alpha byte is ignored on XRGB */
	}
	munmap(pixels, size);

	struct wl_shm_pool *pool = wl_shm_create_pool(client->shm, fd, (int32_t)size);
	client->buffer = wl_shm_pool_create_buffer(pool, 0, client->width, client->height,
			(int32_t)stride, client->argb);
	wl_shm_pool_destroy(pool);
	close(fd);
	if (client->buffer == NULL) {
		say("error the desktop would not take the buffer\n");
		return -1;
	}
	/* The buffer handed over last is left alone rather than destroyed here: a
	 * client may only let go of one the compositor has released, and a test
	 * window that resizes a few times can be allowed to keep a few. */
	wl_surface_attach(client->surface, client->buffer, 0, 0);
	wl_surface_damage(client->surface, 0, 0, client->width, client->height);
	wl_surface_commit(client->surface);
	if (!client->shown) {
		client->shown = 1;
		say("mapped %dx%d\n", client->width, client->height);
	}
	return 0;
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
	shm_format,
};

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	/* A desktop that wonders whether a window is still there gets an answer,
	 * or it takes the window away from a test that is still reading it. */
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	wm_base_ping,
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
		client->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(client->wm_base, &wm_base_listener, client);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
	/* The desktop takes interfaces away while the screen is locked and hands
	 * them back afterwards; a test that cares asks again rather than keeping a
	 * list from before. */
}

static const struct wl_registry_listener registry_listener = {
	registry_global,
	registry_global_remove,
};

/* --------------------------------------------------------------- xdg-shell */

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
	struct client *client = data;
	/* Nothing is drawn before this answer, which is the whole of xdg-shell's
	 * rule about the first configure as well as about later ones. */
	xdg_surface_ack_configure(surface, serial);
	client->configure = serial;
}

static void xdg_surface_handle_destroy(void *data, struct xdg_surface *surface) {
}

static const struct xdg_surface_listener xdg_surface_listener = {
	xdg_surface_configure,
	xdg_surface_handle_destroy,
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
	if (width == client->width && height == client->height) {
		return;
	}
	client->width = width;
	client->height = height;
	say("configure %dx%d\n", width, height);
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
	toplevel_configure,
	toplevel_close,
};

/* ---------------------------------------------------------------- dispatch */

/* Reads until something happens, or the wait runs out, or the test writes to
 * us. Returns 1 to go on, 0 for a timeout of our own, and -1 when the desktop
 * went away mid-conversation. */
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
		(void)write(STDOUT_FILENO, "stop\n", 5);
		client->running = 0;
		return 0;
	}
	if (wl_display_read_events(client->display) < 0) {
		return -1;
	}
	return wl_display_dispatch_pending(client->display) < 0 ? -1 : 1;
}

static void usage(const char *name) {
	printf("Usage:\n"
		"  %s --globals            list the interfaces this desktop offers, and exit\n"
		"  %s --window [options]   map one window and stay until told to stop\n"
		"Options for --window: --title TEXT, --app-id NAME, --idle-ms N (default 30000)\n",
		name, name);
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

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--globals") == 0) {
			globals_only = 1;
		} else if (strcmp(argv[i], "--window") == 0) {
			globals_only = 0;
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
	if (client.compositor == NULL || client.shm == NULL || client.wm_base == NULL) {
		say("error this desktop has no compositor, no shared memory, or no xdg-shell\n");
		return 1;
	}
	if (wl_display_roundtrip(client.display) < 0) {
		say("error the desktop did not say what its shared memory can hold\n");
		return 1;
	}
	if (client.want_argb) {
		client.argb = WL_SHM_FORMAT_ARGB8888;
	} else if (client.want_xrgb) {
		client.argb = WL_SHM_FORMAT_XRGB8888;
	} else {
		say("error this desktop shares memory in no shape we can draw into\n");
		return 1;
	}

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

	int waited = 0;
	while (client.running) {
		if (client.configure) {
			client.configure = 0;
			if (paint(&client) < 0) {
				client.failed = 1;
				break;
			}
		}
		if (client.resize) {
			client.resize = 0;
			if (paint(&client) < 0) {
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
		waited += 200;
		if (waited >= client.idle_ms) {
			say("timeout\n");
			break;
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
