project(
	'tessera',
	'c',
	version: '1.3.0',
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
	'protocols/wlr-layer-shell-unstable-v1.xml',
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
	'src/deco.c',
	'src/idle.c',
	'src/input.c',
	'src/ipc.c',
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

test('idle timing', executable('idle_test', 'tests/idle_test.c', build_by_default: false))
