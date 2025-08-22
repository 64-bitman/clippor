wl_copy = find_program('wl-copy')
wl_paste = find_program('wl-paste')

if not wl_copy.found() or not wl_paste.found()
    subdir_done()
endif

tests = ['clipboard', 'wayland']

foreach suffix : tests
    exe = executable(
        'test_' + suffix,
        'test_' + suffix + '.c',
        'dummy-selection.c',
        'test.c',
        sources,
        include_directories: [includes, include_directories('.')],
        dependencies: deps,
        link_with: [link_with, wayland_library]
        )

    test(
        suffix,
        exe,
        env: [
            'G_TEST_SRCDIR=@0@'.format(meson.current_source_dir()),
            'G_TEST_BUILDDIR=@0@'.format(meson.current_build_dir()),
            ],
        protocol: 'tap',
        )
endforeach
