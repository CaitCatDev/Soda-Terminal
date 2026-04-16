project('project-terminal', 'c')

cc = meson.get_compiler('c')

add_project_arguments(['-I./include/'] + ['-pedantic'] +
                      ['-Wall'] + ['-Wextra'], language : 'c')

if cc.has_argument('-Wstrict-aliasing')
  add_project_arguments(['-Wstrict-aliasing=2'] + ['-fstrict-aliasing'], language: 'c')
endif

wayland_client = dependency('wayland-client', required: get_option('enable-wl'))
xcb = dependency('xcb', required: get_option('enable-x11'))
xcb_shm = dependency('xcb-shm', required: get_option('enable-x11'))
xcb_xkb = dependency('xcb-xkb', required: get_option('enable-x11'))
freetype2 = dependency('freetype2')
harfbuzz = dependency('harfbuzz')
fontconfig = dependency('fontconfig')
xkbcommon = dependency('xkbcommon')
xkbcommon_x11 = dependency('xkbcommon-x11', required: get_option('enable-x11'))

wayland_protocols = dependency('wayland-protocols', required: get_option('enable-wl'))
wayland_scanner = dependency('wayland-scanner', required: get_option('enable-wl'))

if wayland_protocols.found()
  wayland_proto_datadir = wayland_protocols.get_variable('pkgdatadir')
  wayland_scanner_bin = find_program(wayland_scanner.get_variable('wayland_scanner'))
  xdg_shell_protocol_header = custom_target(
    'xdg-shell-client-protocol-header',
    output: '@BASENAME@-client-protocol.h',
    input: wayland_proto_datadir + '/stable/xdg-shell/xdg-shell.xml',
    command: [wayland_scanner_bin, 'client-header', '@INPUT@', '@OUTPUT@']
    )

  xdg_shell_protocol_src = custom_target(
    'xdg-shell-client-protocol-src',
    output: '@BASENAME@-protocol-code.c',
    input: wayland_proto_datadir + '/stable/xdg-shell/xdg-shell.xml',
    command: [wayland_scanner_bin, 'private-code', '@INPUT@', '@OUTPUT@']
    )
endif

srcs = [ './src/main.c' ]
inc = include_directories('./include/')

if xcb.found()
  add_project_arguments(['-DTERM_X11_SUPPORT=1'], language : 'c')
  srcs += './src/x11.c'
endif
if wayland_client.found()
  add_project_arguments(['-DTERM_WL_SUPPORT=1'], language : 'c')
  srcs += [ './src/wayland.c', xdg_shell_protocol_src, xdg_shell_protocol_header ]
endif

executable('project-terminal', srcs, include_directories : inc, dependencies: [ wayland_client, xcb, xcb_xkb, xcb_shm, freetype2, fontconfig, xkbcommon, xkbcommon_x11, harfbuzz ])
