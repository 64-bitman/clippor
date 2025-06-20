lib = static_library(name, sources, test_sources,
  include_directories: [includes, test_includes],
  dependencies: deps,
  link_with: link_with
  )

env =  [
  'G_TEST_SRCDIR=@0@'.format(meson.current_source_dir()),
  'G_TEST_BUILDDIR=@0@'.format(meson.current_build_dir()),
  ]
protocol = 'tap'
suite = ['wayland']

tests = ['wayland-connection', 'wayland-seat']

foreach name: tests
    bin = executable(
      name,
      name + '.c',
      dependencies: deps,
      include_directories: [includes, test_includes],
      link_with: lib)

    test(
      name,
      bin,
      env: env,
      protocol: protocol,
      suite: suite)
endforeach
