project(
	'tessera',
	'c',
	version: files('../VERSION'),
	license: 'MIT',
	meson_version: '>=0.60.0',
	default_options: ['c_std=c11', 'warning_level=2', 'werror=false'],
)

add_project_arguments(
	'-DWLR_USE_UNSTABLE',
	'-Wno-unused-parameter',
	'-Wno-missing-field-initializers',
	language: 'c',
)

cc = meson.get_compiler('c')

wlroots = dependency('wlroots-0.18', version: ['>=0.18.0', '<0.19.0'])
wayland_server = dependency('wayland-server')
wayland_protos = dependency('wayland-protocols', version: '>=1.32')
wayland_scanner = dependency('wayland-scanner', native: true)
xkbcommon = dependency('xkbcommon')
libinput = dependency('libinput')
pixman = dependency('pixman-1')
cairo = dependency('cairo')
pangocairo = dependency('pangocairo')
libdrm = dependency('libdrm').partial_dependency(compile_args: true, includes: true)
libsystemd = dependency('libsystemd')
pam = cc.find_library('pam')
math = cc.find_library('m')

have_xwayland = wlroots.get_variable(pkgconfig: 'have_xwayland', default_value: 'false') == 'true'
xcb = dependency('xcb', required: have_xwayland)
add_project_arguments('-DTESSERA_XWAYLAND=@0@'.format(have_xwayland ? 1 : 0), language: 'c')
add_project_arguments('-DTESSERA_VERSION="@0@"'.format(meson.project_version()), language: 'c')

scanner = find_program(wayland_scanner.get_variable(pkgconfig: 'wayland_scanner'), native: true)
protocols_dir = wayland_protos.get_variable(pkgconfig: 'pkgdatadir')

protocols = [
	protocols_dir / 'stable/xdg-shell/xdg-shell.xml',
	protocols_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
	protocols_dir / 'unstable/tablet/tablet-unstable-v2.xml',
	protocols_dir / 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
	'protocols/wlr-layer-shell-unstable-v1.xml',
	'protocols/wlr-output-management-unstable-v1.xml',
]

protocol_headers = []
foreach xml : protocols
	protocol_headers += custom_target(
		xml.underscorify() + '_h',
		input: xml,
		output: '@BASENAME@-protocol.h',
		command: [scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
	)
endforeach

sources = files(
	'src/config.c',
	'src/config_file.c',
	'src/deco.c',
	'src/idle.c',
	'src/input.c',
	'src/ipc.c',
	'src/json.c',
	'src/layer.c',
	'src/layout.c',
	'src/lock.c',
	'src/logind.c',
	'src/main.c',
	'src/nightlight.c',
	'src/output.c',
	'src/switcher.c',
	'src/view.c',
	'src/wallpaper.c',
)
if have_xwayland
	sources += files('src/xwayland.c')
endif

executable(
	'tessera',
	sources + protocol_headers,
	dependencies: [wlroots, wayland_server, xkbcommon, libinput, pixman, cairo,
		pangocairo, libdrm, libsystemd, pam, math, xcb],
	install: true,
)

# A window for the desktop's own tests to look at, and a listing of what the
# compositor offers whoever asks. The tests in tests/test_compositor.py run it:
# without something inside the desktop, the parts that only show up when a client
# is looking - tiling a window, taking an interface away while the screen is
# locked, closing a window from the socket - can be read but not checked. Built
# beside the window manager, and installed nowhere.
wayland_client = dependency('wayland-client')
xdg_shell_xml = protocols_dir / 'stable/xdg-shell/xdg-shell.xml'

executable(
	'tessera-test-window',
	[
		'tests/window_client.c',
		custom_target(
			'xdg-shell client header',
			input: xdg_shell_xml,
			output: 'xdg-shell-client-protocol.h',
			command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
		),
		custom_target(
			'xdg-shell client code',
			input: xdg_shell_xml,
			output: 'xdg-shell-protocol.c',
			command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
		),
	],
	dependencies: [wayland_client],
	build_by_default: true,
	install: false,
)

test('idle timing', executable('idle_test', 'tests/idle_test.c', build_by_default: false))
test('reading settings', executable('config_test', 'tests/config_test.c', 'src/config_file.c',
	dependencies: math, build_by_default: false),
	args: [files('../tests/config-sample')])
test('JSON text', executable('json_test', 'tests/json_test.c', 'src/json.c',
	build_by_default: false))
test('text buffers', executable('buf_test', 'tests/buf_test.c', build_by_default: false))
test('tiling', executable('tile_test', 'tests/tile_test.c',
	dependencies: wlroots.partial_dependency(compile_args: true, includes: true),
	build_by_default: false))
