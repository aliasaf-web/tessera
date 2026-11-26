#define _POSIX_C_SOURCE 200809L
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v1.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>
#include "server.h"

void server_spawn(const char *cmd) {
	if (cmd == NULL || cmd[0] == '\0') {
		return;
	}
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "fork failed");
		return;
	}
	if (pid == 0) {
		/* Double fork so children are re-parented to init and never
		 * become zombies of the compositor. */
		setsid();
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		pid_t child = fork();
		if (child == 0) {
			execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		_exit(child < 0 ? 1 : 0);
	}
	waitpid(pid, NULL, 0);
}

static void handle_activate(struct wl_listener *listener, void *data) {
	struct ts_server *server = wl_container_of(listener, server, request_activate);
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	struct ts_view *view = view_from_surface(server, event->surface);
	if (view == NULL || !view->mapped) {
		return;
	}
	/* Only bring a window forward when the request comes from something the
	 * user did (wlroots has checked the click or key press it names), so an
	 * app can't take the keyboard in the middle of typing. */
	if (event->token->seat == NULL) {
		wlr_log(WLR_DEBUG, "Ignored a focus request that the user did not cause");
		/* Still show that the window wants attention. */
		if (view_is_hidden(view) && !view->urgent) {
			view->urgent = true;
			ipc_send_state(server);
		}
		return;
	}
	view_focus_id(server, view->id);
}

static bool is_privileged(struct ts_server *server, const struct wl_global *global) {
	return global == server->security_context->global ||
		global == server->screencopy->global ||
		global == server->export_dmabuf->global ||
		global == server->data_control->global ||
		global == server->virtual_keyboard->global ||
		global == server->virtual_pointer->global ||
		global == server->layer_shell->global ||
		global == server->output_manager->global;
}

static bool filter_global(const struct wl_client *client, const struct wl_global *global,
		void *data) {
	struct ts_server *server = data;
	/* While the screen is locked, nothing that starts now may watch the screen,
	 * read the clipboard or rearrange the monitors: those names are not in the
	 * list a new client is given, so it never learns they exist. Nothing queues
	 * the bind for later - the interface simply reappears with the desktop.
	 * A program that was already running keeps what it took
	 * before the lock - taking a capability away from something already connected
	 * is not something a compositor can do without killing it, so the README says
	 * what the lock screen does and does not defend against instead of promising
	 * more than it can give.
	 * Virtual keyboards and pointers are deliberately not held back: someone with
	 * an on-screen keyboard or a switch device has to be able to reach the
	 * password field, and one of those cannot invent a password you did not type. */
	if (server->locked && (global == server->screencopy->global ||
			global == server->export_dmabuf->global ||
			global == server->data_control->global ||
			global == server->output_manager->global)) {
		return false;
	}
	/* Apps started in a sandbox connect through a security context. */
	bool sandboxed = wlr_security_context_manager_v1_lookup_client(
		server->security_context, client) != NULL;
	return !sandboxed || !is_privileged(server, global);
}

static int handle_terminate(int signal_number, void *data) {
	struct ts_server *server = data;
	wl_display_terminate(server->display);
	return 0;
}

static void usage(const char *name) {
	printf("Tessera %s - a clean, simple desktop\n\n"
		"Usage:\n"
		"  %s                 start the desktop\n"
		"  %s --locked        start the desktop with the lock screen showing\n"
		"  %s msg COMMAND     send a command to the running desktop\n"
		"  %s --version       show the version\n",
		TESSERA_VERSION, name, name, name, name);
}

int main(int argc, char *argv[]) {
	if (argc >= 2 && strcmp(argv[1], "msg") == 0) {
		return ipc_client_main(argc - 2, argv + 2);
	}
	if (argc >= 2 && strcmp(argv[1], "--auth-helper") == 0) {
		return lock_auth_helper_main();
	}
	if (argc >= 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
		printf("tessera %s\n", TESSERA_VERSION);
		return 0;
	}
	bool start_locked = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--locked") == 0) {
			start_locked = true;
		} else {
			usage(argv[0]);
			return strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0 ? 0 : 1;
		}
	}

	wlr_log_init(getenv("TESSERA_DEBUG") ? WLR_DEBUG : WLR_INFO, NULL);
	signal(SIGPIPE, SIG_IGN);

	struct ts_server server = {0};
	server.ipc_fd = -1;
	server.auth_fd = -1;
	server.sleep_inhibit_fd = -1;
	config_load(&server);
	theme_from_config(&server);

	server.display = wl_display_create();
	server.loop = wl_display_get_event_loop(server.display);
	/* Logging out sends SIGTERM; close down properly instead of just dying. */
	struct wl_event_source *sigterm =
		wl_event_loop_add_signal(server.loop, SIGTERM, handle_terminate, &server);
	struct wl_event_source *sigint =
		wl_event_loop_add_signal(server.loop, SIGINT, handle_terminate, &server);
	server.backend = wlr_backend_autocreate(server.loop, &server.session);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "Could not start: no display or input devices are available");
		return 1;
	}

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "Could not create a renderer");
		return 1;
	}
	wlr_renderer_init_wl_shm(server.renderer, server.display);
	if (wlr_renderer_get_texture_formats(server.renderer, WLR_BUFFER_CAP_DMABUF) != NULL) {
		struct wlr_linux_dmabuf_v1 *dmabuf =
			wlr_linux_dmabuf_v1_create_with_renderer(server.display, 4, server.renderer);
		server.scene = wlr_scene_create();
		wlr_scene_set_linux_dmabuf_v1(server.scene, dmabuf);
	} else {
		server.scene = wlr_scene_create();
	}

	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "Could not create an allocator");
		return 1;
	}

	server.compositor = wlr_compositor_create(server.display, 6, server.renderer);
	wlr_subcompositor_create(server.display);
	wlr_data_device_manager_create(server.display);
	wlr_primary_selection_v1_device_manager_create(server.display);
	server.data_control = wlr_data_control_manager_v1_create(server.display);
	wlr_viewporter_create(server.display);
	wlr_single_pixel_buffer_manager_v1_create(server.display);
	wlr_fractional_scale_manager_v1_create(server.display, 1);
	wlr_presentation_create(server.display, server.backend);
	server.screencopy = wlr_screencopy_manager_v1_create(server.display);
	server.export_dmabuf = wlr_export_dmabuf_manager_v1_create(server.display);
	wlr_alpha_modifier_v1_create(server.display);
	server.pointer_gestures = wlr_pointer_gestures_v1_create(server.display);
	struct wlr_xdg_foreign_registry *foreign = wlr_xdg_foreign_registry_create(server.display);
	wlr_xdg_foreign_v1_create(server.display, foreign);
	wlr_xdg_foreign_v2_create(server.display, foreign);

	/* Ask older (GTK3/Qt) apps to let us draw their title bars. */
	server.kde_decoration = wlr_server_decoration_manager_create(server.display);
	wlr_server_decoration_manager_set_default_mode(server.kde_decoration,
		WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);

	server.output_layout = wlr_output_layout_create(server.display);
	wlr_xdg_output_manager_v1_create(server.display, server.output_layout);
	server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

	struct wlr_scene_tree *root = &server.scene->tree;
	server.layer_background = wlr_scene_tree_create(root);
	server.layer_shell_bg = wlr_scene_tree_create(root);
	server.layer_shell_bottom = wlr_scene_tree_create(root);
	server.layer_tiled = wlr_scene_tree_create(root);
	server.layer_floating = wlr_scene_tree_create(root);
	server.layer_shell_top = wlr_scene_tree_create(root);
	server.layer_fullscreen = wlr_scene_tree_create(root);
	server.layer_popups = wlr_scene_tree_create(root);
	server.layer_shell_overlay = wlr_scene_tree_create(root);
	server.layer_drag = wlr_scene_tree_create(root);
	server.layer_lock = wlr_scene_tree_create(root);

	server.idle_notifier = wlr_idle_notifier_v1_create(server.display);

	server.xdg_activation = wlr_xdg_activation_v1_create(server.display);
	server.request_activate.notify = handle_activate;
	wl_signal_add(&server.xdg_activation->events.request_activate, &server.request_activate);

	output_init(&server);
	layout_init(&server);
	views_init(&server);
	layers_init(&server);
	input_init(&server);

	server.security_context = wlr_security_context_manager_v1_create(server.display);
	wl_display_set_global_filter(server.display, filter_global, &server);

	const char *socket = wl_display_add_socket_auto(server.display);
	if (socket == NULL) {
		wlr_log(WLR_ERROR, "Could not create the Wayland socket");
		wlr_backend_destroy(server.backend);
		return 1;
	}
	server.socket_name = socket;
	/* Before the screens appear: setting them up sends events. */
	ipc_init(&server);

	if (!wlr_backend_start(server.backend)) {
		wlr_log(WLR_ERROR, "Could not start the backend");
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, true);
	setenv("XDG_CURRENT_DESKTOP", "Tessera", true);
	setenv("XDG_SESSION_DESKTOP", "tessera", true);
	setenv("XDG_SESSION_TYPE", "wayland", true);
	/* Lets helper scripts tell when this desktop has ended. */
	char pid[16];
	snprintf(pid, sizeof(pid), "%ld", (long)getpid());
	setenv("TESSERA_PID", pid, true);

#if TESSERA_XWAYLAND
	xwayland_init(&server);
#endif
	night_light_init(&server);
	lock_init(&server);
	idle_init(&server);
	logind_init(&server);

	if (start_locked) {
		lock_engage(&server);
	}

	wlr_log(WLR_INFO, "Tessera is running on WAYLAND_DISPLAY=%s", socket);

	const char *startup = getenv("TESSERA_STARTUP");
	server_spawn(startup ? startup : "tessera-autostart");

	wl_display_run(server.display);
	wlr_log(WLR_INFO, "Tessera is closing");

	wl_event_source_remove(sigterm);
	wl_event_source_remove(sigint);
	/* Leave the screens as we found them: the login screen that follows is
	 * not ours to redraw, and it should not come up blanked or tinted. */
	output_set_dpms(&server, true);
	server.night_override = -1;
	night_light_update(&server, true);
	ipc_finish(&server);
	wl_display_destroy_clients(server.display);
#if TESSERA_XWAYLAND
	if (server.xwayland) {
		wlr_xwayland_destroy(server.xwayland);
	}
#endif
	/* Screens and input devices go first: their clean-up still uses the scene. */
	wlr_backend_destroy(server.backend);
	wlr_scene_node_destroy(&server.scene->tree.node);
	deco_forget_corners(&server);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wl_display_destroy(server.display);
	return 0;
}
