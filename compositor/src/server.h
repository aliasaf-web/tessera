#ifndef TESSERA_SERVER_H
#define TESSERA_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <xkbcommon/xkbcommon.h>

#if TESSERA_XWAYLAND
#include <wlr/xwayland.h>
#endif

#define TS_WORKSPACES 9

/* Every scene node that belongs to something we manage carries a pointer to
 * one of these headers in node->data, so hit-testing can find its owner. */
enum ts_node_kind {
	TS_NODE_VIEW,
	TS_NODE_LAYER,
	TS_NODE_LOCK,
};

struct ts_node {
	enum ts_node_kind kind;
};

struct ts_color {
	float r, g, b, a;
};

struct ts_theme {
	bool dark;
	struct ts_color background;
	struct ts_color surface;
	struct ts_color surface_hover;
	struct ts_color text;
	struct ts_color text_dim;
	struct ts_color accent;
	struct ts_color on_accent;
};

struct ts_config {
	char theme[16];
	char accent[16];
	char kb_layout[64];
	char kb_variant[64];
	char kb_options[128];
	char kb_model[64];
	bool tap_to_click;
	bool natural_scroll;
	double scale; /* 0 = automatic */
	int gaps;
	int idle_lock_minutes;
	int idle_screen_off_minutes;
	bool lock_on_start;
	char terminal[256];
	char files[256];
	char browser[256];
};

enum ts_cursor_mode {
	TS_CURSOR_PASSTHROUGH,
	TS_CURSOR_MOVE,          /* moving a floating window */
	TS_CURSOR_RESIZE,        /* resizing a floating window */
	TS_CURSOR_SWAP,          /* dragging a tiled window onto another */
	TS_CURSOR_RATIO,         /* dragging the gap between columns */
	TS_CURSOR_CLOSE_PRESS,   /* pressed the close button, waiting for release */
};

enum ts_view_type {
	TS_VIEW_XDG,
	TS_VIEW_XWAYLAND,
};

struct ts_output {
	struct wl_list link;
	struct ts_server *server;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	struct wlr_scene_rect *background;
	struct wlr_box full_area;   /* layout coords */
	struct wlr_box usable_area; /* layout coords, minus panels */
	int workspace;              /* 1..TS_WORKSPACES */
	double master_ratio[TS_WORKSPACES + 1];
	struct wl_list layers[4];   /* ts_layer_surface.link, by layer */
	struct ts_lock_surface *lock_surface;
	bool dpms_off;

	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

struct ts_view {
	struct ts_node node;
	struct wl_list link;       /* ts_server.views, tiling order */
	struct wl_list focus_link; /* ts_server.focus_stack, most recent first */
	struct ts_server *server;
	enum ts_view_type type;

	struct wlr_xdg_toplevel *xdg_toplevel;
#if TESSERA_XWAYLAND
	struct wlr_xwayland_surface *xsurface;
#endif
	struct wlr_surface *surface; /* main surface, valid while mapped */

	struct wlr_scene_tree *tree;         /* outer container */
	struct wlr_scene_buffer *frame;      /* title bar + outline */
	struct wlr_scene_tree *content_tree; /* client surfaces */
	struct wlr_scene_buffer *corners[4]; /* rounded corner masks */
	struct wlr_scene_tree *popup_tree;   /* xdg popups, kept above windows */
	struct wlr_scene_rect *fs_bg;        /* black backdrop when fullscreen */
	int requested_w, requested_h;        /* last size we asked the client for */

	struct ts_output *output;
	int workspace;
	bool mapped;
	bool floating;
	bool fullscreen;
	bool maximized;
	bool ssd;         /* we draw the title bar */
	bool frame_dirty;
	bool urgent;

	struct wlr_box box;        /* outer box in layout coords */
	struct wlr_box float_box;  /* remembered floating position */
	struct wlr_box content;    /* content box relative to box */
	int frame_w, frame_h, frame_focused, frame_hover_close;
	double frame_scale;

	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	struct wl_listener decoration_request_mode;
	struct wl_listener decoration_destroy;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener request_minimize;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
#if TESSERA_XWAYLAND
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener request_configure;
	struct wl_listener request_activate;
	struct wl_listener set_hints;
	struct wl_listener request_resize_override;
#endif
};

struct ts_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct ts_layer_surface {
	struct ts_node node;
	struct wl_list link;
	struct ts_server *server;
	struct ts_output *output;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene;
	struct wlr_scene_tree *popup_tree;
	bool mapped;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener new_popup;
};

struct ts_keyboard {
	struct wl_list link;
	struct ts_server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

struct ts_lock_surface {
	struct ts_node node;
	struct ts_output *output;
	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer *buffer;
};

struct ts_ipc_client {
	struct wl_list link;
	struct ts_server *server;
	int fd;
	struct wl_event_source *source;
	bool subscribed;
	char buf[1024];
	size_t len;
};

struct ts_server {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend;
	struct wlr_session *session;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_output_layout *output_layout;
	struct wl_list outputs;

	/* Scene stacking order, bottom to top. */
	struct wlr_scene_tree *layer_background;
	struct wlr_scene_tree *layer_shell_bg;
	struct wlr_scene_tree *layer_shell_bottom;
	struct wlr_scene_tree *layer_tiled;
	struct wlr_scene_tree *layer_floating;
	struct wlr_scene_tree *layer_fullscreen;
	struct wlr_scene_tree *layer_shell_top;
	struct wlr_scene_tree *layer_popups;
	struct wlr_scene_tree *layer_shell_overlay;
	struct wlr_scene_tree *layer_drag;
	struct wlr_scene_tree *layer_lock;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wlr_layer_shell_v1 *layer_shell;
	struct wl_listener new_layer_surface;
	struct wlr_xdg_decoration_manager_v1 *xdg_decoration;
	struct wlr_server_decoration_manager *kde_decoration;
	struct wl_listener new_decoration;
	struct wlr_xdg_activation_v1 *xdg_activation;
	struct wl_listener request_activate;
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit;
	struct wl_listener new_inhibitor;
	struct wlr_relative_pointer_manager_v1 *relative_pointer;
	struct wlr_cursor_shape_manager_v1 *cursor_shape;
	struct wl_listener request_set_shape;
#if TESSERA_XWAYLAND
	struct wlr_xwayland *xwayland;
	struct wl_listener xwayland_ready;
	struct wl_listener new_xwayland_surface;
#endif

	struct wl_list views;
	struct wl_list focus_stack;
	struct ts_view *focused_view;
	struct ts_layer_surface *focused_layer;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	const char *cursor_image;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;
	struct wl_list keyboards;
	struct wl_listener new_output;

	enum ts_cursor_mode cursor_mode;
	struct ts_view *grab_view;
	double grab_x, grab_y;
	struct wlr_box grab_box;
	uint32_t resize_edges;
	struct ts_view *swap_target;
	struct wlr_scene_rect *drop_hint;
	struct wlr_scene_tree *drag_icons;
	struct ts_view *hover_view; /* view whose close button is hovered */
	struct ts_output *ratio_output;

	bool super_alone;
	uint32_t super_keycode;

	/* Lock screen */
	bool locked;
	char password[256];
	size_t password_len;
	bool auth_busy;
	bool auth_failed;
	pid_t auth_pid;
	int auth_fd;
	struct wl_event_source *auth_source;
	struct wl_event_source *clock_timer;

	/* Idle */
	struct wl_event_source *idle_timer;
	int idle_stage;

	/* IPC */
	int ipc_fd;
	char ipc_path[108];
	struct wl_event_source *ipc_source;
	struct wl_list ipc_clients;

	/* logind */
	void *bus;
	struct wl_event_source *bus_source;
	int sleep_inhibit_fd;

	struct ts_config config;
	struct ts_theme theme;
	char *socket_name;
};

/* main.c */
void server_spawn(const char *cmd);

/* config.c */
void config_load(struct ts_server *server);
void theme_from_config(struct ts_server *server);

/* output.c */
void output_init(struct ts_server *server);
struct ts_output *output_at(struct ts_server *server, double lx, double ly);
struct ts_output *output_focused(struct ts_server *server);
void output_set_dpms(struct ts_server *server, bool on);

/* layer.c */
void layers_init(struct ts_server *server);
void arrange_layers(struct ts_output *output);
struct ts_layer_surface *layer_wants_exclusive_focus(struct ts_server *server);

/* view.c */
void views_init(struct ts_server *server);
void view_focus(struct ts_view *view);
void view_close(struct ts_view *view);
void view_set_fullscreen(struct ts_view *view, bool fullscreen);
void view_set_maximized(struct ts_view *view, bool maximized);
void view_set_floating(struct ts_view *view, bool floating);
void view_move_to_workspace(struct ts_view *view, int workspace);
void view_set_box(struct ts_view *view, struct wlr_box box);
void view_update_frame(struct ts_view *view);
void view_raise(struct ts_view *view);
const char *view_title(struct ts_view *view);
const char *view_app_id(struct ts_view *view);
struct ts_view *view_from_surface(struct ts_server *server, struct wlr_surface *surface);
void view_map_common(struct ts_view *view);
void popup_create(struct wlr_xdg_popup *xdg_popup, struct wlr_scene_tree *parent_tree);
void view_unmap_common(struct ts_view *view);
void view_destroy_common(struct ts_view *view);
struct ts_view *view_create_common(struct ts_server *server, enum ts_view_type type);
void view_get_geometry(struct ts_view *view, struct wlr_box *geo);
void view_send_size(struct ts_view *view, int width, int height);
void view_fit_floating(struct ts_view *view);
void views_redraw_frames(struct ts_server *server);
void focus_top_view(struct ts_server *server);
#if TESSERA_XWAYLAND
void xwayland_init(struct ts_server *server);
#endif

/* layout.c */
void arrange_output(struct ts_output *output);
void arrange_all(struct ts_server *server);
void workspace_switch(struct ts_server *server, int workspace);
bool view_visible(struct ts_view *view);
struct ts_view *view_in_direction(struct ts_server *server, struct ts_view *from, int dx, int dy);
void view_swap(struct ts_view *a, struct ts_view *b);
bool layout_gap_hit(struct ts_output *output, double lx, double ly);
void layout_set_ratio_from_cursor(struct ts_output *output, double lx);

/* input.c */
void input_init(struct ts_server *server);
void cursor_rebase(struct ts_server *server);
void seat_focus_surface(struct ts_server *server, struct wlr_surface *surface);
void keyboard_apply_config(struct ts_server *server);
void notify_activity(struct ts_server *server);
void input_begin_move(struct ts_view *view);
void input_begin_resize(struct ts_view *view, uint32_t edges);
void input_view_gone(struct ts_view *view);

/* deco.c */
struct wlr_buffer *deco_render_frame(struct ts_server *server, int w, int h, double scale,
	const char *title, bool focused, bool ssd, bool hover_close, bool floating);
struct wlr_buffer *deco_render_corner(struct ts_server *server, int corner, double scale,
	bool focused, bool ssd);
struct wlr_buffer *deco_render_lock(struct ts_server *server, int w, int h, double scale);
int deco_titlebar_height(void);
int deco_border(void);
int deco_radius(void);
bool deco_close_hit(int frame_w, double x, double y);

/* ipc.c */
void ipc_init(struct ts_server *server);
void config_reload(struct ts_server *server);
void ipc_finish(struct ts_server *server);
void ipc_broadcast(struct ts_server *server, const char *json_line);
void ipc_send_state(struct ts_server *server);
int ipc_client_main(int argc, char **argv);

/* lock.c */
void lock_init(struct ts_server *server);
int lock_auth_helper_main(void);
void lock_engage(struct ts_server *server);
bool lock_handle_key(struct ts_server *server, xkb_keysym_t sym, uint32_t codepoint);
void lock_arrange(struct ts_server *server);
void lock_output_destroyed(struct ts_output *output);

/* idle.c */
void idle_init(struct ts_server *server);
void idle_reset(struct ts_server *server);
void idle_update_inhibit(struct ts_server *server);

/* logind.c */
void logind_init(struct ts_server *server);
void logind_release_sleep_lock(struct ts_server *server);

#endif
