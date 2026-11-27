#ifndef TESSERA_SERVER_H
#define TESSERA_SERVER_H

/* The longest password this screen can hand to PAM. PAM takes a response of
 * PAM_MAX_RESP_SIZE bytes counting the terminating zero (512, which is why the
 * number below is 511 usable), so a longer one cannot be checked at all -
 * which the lock screen says out loud rather than testing a shortened version
 * of it and reporting "wrong password". */
#define TS_PASSWORD_MAX 512

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
#include "config.h"
#include "json.h"

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

/* The buttons at the bottom of the lock screen. */
enum ts_lock_button {
	TS_LOCK_BUTTON_NONE,
	TS_LOCK_BUTTON_SLEEP,
	TS_LOCK_BUTTON_RESTART,
	TS_LOCK_BUTTON_SHUT_DOWN,
	TS_LOCK_BUTTON_COUNT,
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

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif


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
	struct wlr_box lock_buttons[TS_LOCK_BUTTON_COUNT]; /* output coords */
	bool dpms_off;
	bool in_layout; /* in use: in ts_server.outputs; otherwise in spare_outputs */
	bool off;       /* switched off in the settings */
	bool mirror;    /* showing the same picture as the main screen */
	struct wlr_scene_buffer *wallpaper;
	void *wallpaper_surface; /* cairo_surface_t, for the rounded corners */
	int wallpaper_w, wallpaper_h;
	double wallpaper_scale;

	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

struct ts_view {
	struct ts_node node;
	uint32_t id;               /* stable number used by the switcher */
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
	struct wlr_scene_tree *frame;        /* background, outline and title bar */
	struct wlr_scene_rect *frame_bg;     /* behind the window contents */
	struct wlr_scene_rect *edges[4];     /* outline: top, right, bottom, left */
	struct wlr_scene_buffer *titlebar;   /* title and close button */
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
	bool ssd;           /* we draw the title bar */
	bool frame_dirty;   /* title bar needs drawing again */
	bool corners_dirty; /* corners need drawing again */
	bool urgent;

	struct wlr_box box;        /* outer box in layout coords */
	struct wlr_box float_box;  /* remembered floating position */
	struct wlr_box content;    /* content box relative to box */
	int frame_w, frame_focused, frame_hover_close;
	struct wlr_box corner_box; /* where the corners were drawn for */
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
	struct wl_listener set_override_redirect;
#endif
};

struct ts_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener reposition;
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

struct ts_pointer {
	struct wl_list link;
	struct ts_server *server;
	struct wlr_input_device *device;
	struct wl_listener destroy;
};

struct ts_keyboard {
	struct wl_list link;
	struct ts_server *server;
	struct wlr_keyboard *wlr_keyboard;
	bool is_virtual; /* made by an app, not a real keyboard */
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

/* The lock screen's parts that are drawn as images. */
enum ts_lock_part {
	TS_LOCK_PART_CLOCK,
	TS_LOCK_PART_FIELD,   /* password field, message and keyboard notes */
	TS_LOCK_PART_BUTTONS,
	TS_LOCK_PART_COUNT,
};

struct ts_lock_surface {
	struct ts_node node;
	struct ts_output *output;
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *backdrop;     /* plain colour under everything */
	struct wlr_scene_buffer *wallpaper;  /* dimmed wallpaper, if there is one */
	struct wlr_scene_buffer *parts[TS_LOCK_PART_COUNT];
};

/* Fingers followed at once on a touch screen. */
#define TS_TOUCH_POINTS 10

struct ts_touch_point {
	int32_t id;
	double ox, oy; /* layout position of the surface it touches */
};

/* Longest request line, including its newline. */
#define TS_IPC_LINE_MAX 1024

struct ts_ipc_client {
	struct wl_list link;
	struct ts_server *server;
	int fd;
	struct wl_event_source *source;
	bool subscribed;
	uint64_t state_hash; /* the state it was last sent */
	bool discarding; /* skipping the rest of a request that was too long */
	char buf[TS_IPC_LINE_MAX];
	size_t len;
	char *out;     /* lines waiting until the socket can take them */
	size_t out_len;
	size_t out_cap;
	bool waiting_to_write;
	bool closing;  /* disconnected for not reading; waiting for the hang-up */
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
	struct wl_list outputs;       /* screens in use (ts_output.link) */
	struct wl_list spare_outputs; /* switched off or mirroring */
	struct wlr_output_manager_v1 *output_manager;
	struct wl_listener output_manager_apply;
	struct wl_listener output_manager_test;
	struct wl_listener layout_change;

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
	/* Protocols that can read the screen or the clipboard, fake input, draw
	 * desktop panels or change the screens (output_manager, above). Sandboxed
	 * apps (such as Flatpaks) don't get them. */
	struct wlr_security_context_manager_v1 *security_context;
	struct wlr_screencopy_manager_v1 *screencopy;
	struct wlr_export_dmabuf_manager_v1 *export_dmabuf;
	struct wlr_data_control_manager_v1 *data_control;
	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard;
	struct wl_listener new_virtual_keyboard;
	struct wlr_virtual_pointer_manager_v1 *virtual_pointer;
	struct wl_listener new_virtual_pointer;
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit;
	struct wl_listener new_inhibitor;
	struct wlr_relative_pointer_manager_v1 *relative_pointer;
	struct wlr_pointer_constraints_v1 *pointer_constraints;
	struct wlr_pointer_constraint_v1 *active_constraint; /* holding the pointer now */
	struct wl_listener new_constraint;
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
	/* The screen in use: where the focused window is, or the screen picked
	 * last (by changing its workspace or clicking its desktop). */
	struct ts_output *active_output;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	const char *cursor_image;

	/* Touch screens: fingers on apps that take touch, with where each app's
	 * surface starts; a finger anywhere else acts as the mouse. */
	struct wl_listener touch_down;
	struct wl_listener touch_up;
	struct wl_listener touch_motion;
	struct wl_listener touch_cancel;
	struct wl_listener touch_frame;
	struct ts_touch_point touch_points[TS_TOUCH_POINTS];
	int touch_count;
	bool touch_as_mouse;
	int32_t touch_mouse_id; /* the finger acting as the mouse */
	/* Drawing tablets: the pen moves the pointer and clicks. */
	struct wl_listener tablet_axis;
	struct wl_listener tablet_proximity;
	struct wl_listener tablet_tip;
	struct wl_listener tablet_button;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;
	struct wl_list keyboards;
	struct wl_list pointers;
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
	struct ts_view *title_click_view; /* for spotting a double click */
	uint32_t title_click_time;
	uint32_t next_view_id;

	/* Three-finger swipes */
	struct wlr_pointer_gestures_v1 *pointer_gestures;
	struct wl_listener swipe_begin;
	struct wl_listener swipe_update;
	struct wl_listener swipe_end;
	struct wl_listener pinch_begin;
	struct wl_listener pinch_update;
	struct wl_listener pinch_end;
	struct wl_listener hold_begin;
	struct wl_listener hold_end;
	bool swipe_active;
	double swipe_dx, swipe_dy;

	/* Workspace slide animation */
	struct {
		bool active;
		struct ts_output *output;
		int from, to, dir;
		uint32_t start_ms;
		struct wl_event_source *timer;
	} ws_anim;

	/* Alt+Tab window switcher */
	bool switcher_active;
	uint32_t switcher_mod;
	uint32_t switcher_ids[32];
	int switcher_count;
	int switcher_sel;
	uint32_t switcher_prev_id;

	/* Lock screen */
	bool locked;
	char password[TS_PASSWORD_MAX];
	size_t password_len;
	bool password_long;      /* more was typed than the field can hold */
	bool auth_busy;
	bool auth_failed;
	bool auth_error;         /* the check itself went wrong, not the password */
	bool auth_refused;       /* the password was right; the account may not be used */
	int auth_failures;       /* wrong passwords in a row */
	int auth_wait;           /* seconds before the next try is allowed */
	struct wl_event_source *auth_wait_timer;
	struct wl_event_source *auth_timeout; /* a check that never answers */
	pid_t auth_pid;
	int auth_fd;
	struct wl_event_source *auth_source;
	struct wl_event_source *clock_timer;
	enum ts_lock_button lock_hover;
	enum ts_lock_button lock_armed;  /* clicked once; a second click confirms */
	struct wl_event_source *lock_arm_timer;
	bool lock_caps;                  /* Caps Lock was on when last drawn */
	int lock_layout;                 /* keyboard layout when last drawn */

	/* Night light */
	bool night_active;
	int night_override;      /* 0 follow the schedule, 1 on, -1 off */
	bool night_scheduled;
	struct wl_event_source *night_timer;

	/* Idle */
	struct wl_event_source *idle_timer;
	int idle_elapsed;      /* minutes without input when the timer last fired */
	int idle_next;         /* minutes without input when it fires next */
	bool idle_screen_off;  /* the screen was turned off for being idle */

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
	/* Without a wallpaper every window's rounded corners look the same, so
	 * they are drawn once: [focused][corner], for corner_cache_scale. */
	struct wlr_buffer *corner_cache[2][4];
	double corner_cache_scale;
	const char *socket_name; /* owned by the display */
};

/* main.c */
void server_spawn(const char *cmd);

/* config.c */
void config_load(struct ts_server *server);
void theme_from_config(struct ts_server *server);
bool output_is_compact(struct ts_output *output);
int output_gaps(struct ts_output *output);

/* output.c */
void output_init(struct ts_server *server);
struct ts_output *output_at(struct ts_server *server, double lx, double ly);
struct ts_output *output_by_name(struct ts_server *server, const char *name);
struct ts_output *output_builtin(struct ts_server *server);
struct ts_output *output_focused(struct ts_server *server);
void output_set_dpms(struct ts_server *server, bool on);
void outputs_apply_config(struct ts_server *server);
char *outputs_json(struct ts_server *server);

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
struct ts_view *view_by_id(struct ts_server *server, uint32_t id);
void view_focus_id(struct ts_server *server, uint32_t id);
void focus_output(struct ts_server *server, struct ts_output *output);
void focus_top_view(struct ts_server *server);
#if TESSERA_XWAYLAND
void xwayland_init(struct ts_server *server);
#endif

/* layout.c */
void arrange_output(struct ts_output *output);
void arrange_all(struct ts_server *server);
void workspace_switch(struct ts_server *server, int workspace);
void workspace_switch_output(struct ts_output *output, int workspace);
void workspace_step(struct ts_server *server, int direction);
void workspace_step_output(struct ts_output *output, int direction);
void layout_init(struct ts_server *server);
void layout_output_destroyed(struct ts_output *output);
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
void pointers_apply_config(struct ts_server *server);
void notify_activity(struct ts_server *server);
void input_begin_move(struct ts_view *view);
void input_begin_resize(struct ts_view *view, uint32_t edges);
void input_view_gone(struct ts_view *view);
bool view_is_hidden(struct ts_view *view);
void input_cancel_grab(struct ts_server *server);
void input_constraint_update(struct ts_server *server);
void input_map_devices(struct ts_server *server);

/* deco.c */
struct wlr_buffer *deco_render_titlebar(struct ts_server *server, int w, double scale,
	const char *title, bool focused, bool hover_close, bool floating);
struct ts_color deco_outline_color(struct ts_server *server, bool focused, bool floating,
	int *width);
struct wlr_buffer *deco_render_corner(struct ts_server *server, int corner, double scale,
	bool focused, struct ts_output *output, double ox, double oy);
struct wlr_buffer *deco_shared_corner(struct ts_server *server, int corner, double scale,
	bool focused);
void deco_forget_corners(struct ts_server *server);
struct wlr_buffer *deco_render_lock_backdrop(struct ts_server *server, struct ts_output *output,
	int w, int h, double scale);
struct wlr_buffer *deco_render_lock_part(struct ts_server *server, struct ts_output *output,
	enum ts_lock_part part, int w, int h, double scale, struct wlr_box *box);
struct wlr_buffer *deco_buffer_from_surface(void *cairo_surface);
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

/* switcher.c */
void switcher_step(struct ts_server *server, uint32_t modifier, int delta);
void switcher_finish(struct ts_server *server, bool commit);
void switcher_modifiers(struct ts_server *server, uint32_t modifiers);

/* lock.c */
void lock_init(struct ts_server *server);
int lock_auth_helper_main(void);
void lock_engage(struct ts_server *server);
bool lock_handle_key(struct ts_server *server, xkb_keysym_t sym, uint32_t codepoint);
void lock_handle_motion(struct ts_server *server, double lx, double ly);
void lock_handle_click(struct ts_server *server, double lx, double ly);
void lock_keyboard_changed(struct ts_server *server);
bool lock_caps_lock_on(struct ts_server *server);
const char *lock_layout_name(struct ts_server *server);
void lock_arrange(struct ts_server *server);
void lock_output_destroyed(struct ts_output *output);

/* nightlight.c */
void night_light_init(struct ts_server *server);
void night_light_update(struct ts_server *server, bool force);
void night_light_set(struct ts_server *server, bool on);
void night_light_apply_output(struct ts_output *output);

/* wallpaper.c */
void wallpaper_update(struct ts_server *server);
void wallpaper_output(struct ts_output *output);
void wallpaper_output_destroyed(struct ts_output *output);

/* idle.c */
void idle_init(struct ts_server *server);
void idle_reset(struct ts_server *server);
void idle_update_inhibit(struct ts_server *server);

/* logind.c */
void logind_init(struct ts_server *server);
void logind_release_sleep_lock(struct ts_server *server);
void logind_power_action(struct ts_server *server, const char *method);

#endif
