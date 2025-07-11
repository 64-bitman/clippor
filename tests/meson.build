tests = ['config', 'clipboard']

foreach name : tests
  exe = executable(
    name, 'test_' + name + '.c', 'spawn.c', sources,
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
      'CLIPPOR_BIN=@0@'.format(clippor.full_path())
      ],
    protocol: 'tap',
    workdir: meson.current_source_dir()
    )
endforeach
