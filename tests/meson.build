wl_paste = find_program('wl-paste', required: false)
wl_copy = find_program('wl-copy', required: false)
labwc = find_program('labwc', required: false)

if not wl_paste.found() or not wl_copy.found() or not labwc.found()
  subdir_done()
endif

tests = ['config', 'database', 'wayland', 'dbus', 'clipboard', 'server']
i = 0

foreach name : tests
  exe = executable(
    name, 'test_' + name + '.c', 'test.c', sources,
    include_directories: includes + include_directories('.'),
    dependencies: deps,
    link_with: link_with
    )

  test(
    name,
    exe,
    env: [
      'G_TEST_SRCDIR=@0@'.format(meson.current_source_dir()),
      'G_TEST_BUILDDIR=@0@'.format(meson.current_build_dir()),
      ],
    priority: i,
    protocol: 'tap',
    workdir: meson.current_source_dir(),
    )
  i += 1
endforeach
