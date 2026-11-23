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
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v1.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>
#include "server.h"

#define TESSERA_VERSION "1.0.0"

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
	if (view->workspace != view->output->workspace) {
		workspace_switch(server, view->workspace);
	}
	view_focus(view);
}

static void usage(const char *name) {
	printf("Tessera %s - a clean, simple desktop\n\n"
		"Usage:\n"
		"  %s                 start the desktop\n"
		"  %s msg COMMAND     send a command to the running desktop\n"
		"  %s --version       show the version\n",
		TESSERA_VERSION, name, name, name);
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
	if (argc >= 2) {
		usage(argv[0]);
		return strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 ? 0 : 1;
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
	wlr_data_control_manager_v1_create(server.display);
	wlr_viewporter_create(server.display);
	wlr_single_pixel_buffer_manager_v1_create(server.display);
	wlr_fractional_scale_manager_v1_create(server.display, 1);
	wlr_presentation_create(server.display, server.backend);
	wlr_screencopy_manager_v1_create(server.display);
	wlr_export_dmabuf_manager_v1_create(server.display);
	wlr_alpha_modifier_v1_create(server.display);
	wlr_pointer_gestures_v1_create(server.display);
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
	views_init(&server);
	layers_init(&server);
	input_init(&server);

	const char *socket = wl_display_add_socket_auto(server.display);
	if (socket == NULL) {
		wlr_log(WLR_ERROR, "Could not create the Wayland socket");
		wlr_backend_destroy(server.backend);
		return 1;
	}
	server.socket_name = strdup(socket);

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

#if TESSERA_XWAYLAND
	xwayland_init(&server);
#endif
	ipc_init(&server);
	lock_init(&server);
	idle_init(&server);
	logind_init(&server);

	if (server.config.lock_on_start) {
		lock_engage(&server);
	}

	wlr_log(WLR_INFO, "Tessera is running on WAYLAND_DISPLAY=%s", socket);

	const char *startup = getenv("TESSERA_STARTUP");
	server_spawn(startup ? startup : "tessera-autostart");

	wl_display_run(server.display);

	ipc_finish(&server);
	wl_display_destroy_clients(server.display);
#if TESSERA_XWAYLAND
	if (server.xwayland) {
		wlr_xwayland_destroy(server.xwayland);
	}
#endif
	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.display);
	free(server.socket_name);
	return 0;
}
