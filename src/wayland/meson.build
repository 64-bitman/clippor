wl_sources = [files('wayland-connection.c', 'wayland-seat.c', 'wayland-selection.c')]
wl_includes = [include_directories('include')]
wl_link_with = []

wl_includes += includes

wl_dep = dependency('wayland-client')

subdir('protocol')

wayland_module = shared_module(
    'clippor-wayland',
    wl_sources,
    include_directories: wl_includes,
    dependencies: [wl_dep, glib_dep, gio_unix_dep],
    install: false
    )
