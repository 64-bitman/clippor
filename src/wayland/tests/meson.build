env =  [
  'G_TEST_SRCDIR=@0@'.format(meson.current_source_dir()),
  'G_TEST_BUILDDIR=@0@'.format(meson.current_build_dir()),
  ]
protocol = 'tap'
lib = static_library(name, sources,
  include_directories: includes,
  dependencies: deps,
  link_with: link_with
  )
suite = ['wayland']

wayland_connection = executable(
  'wayland-connection',
  'wayland-connection.c',
  'test_util.c',
  dependencies: deps,
  include_directories: [includes, '.'],
  link_with: lib)
wayland_seat = executable(
  'wayland-seat',
  'wayland-seat.c',
  'test_util.c',
  dependencies: deps,
  include_directories: [includes, '.'],
  link_with: lib)

test(
  'wayland-connection',
  wayland_connection,
  env: env,
  protocol: protocol,
  suite: suite
  )
test(
  'wayland-seat',
  wayland_seat,
  env: env,
  protocol: protocol,
  suite: suite
  )
