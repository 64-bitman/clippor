wl_sources = [files('wayland-connection.c', 'wayland-seat.c', 'wayland-selection.c')]
wl_includes = [include_directories('include')]
wl_link_with = []

wl_includes += includes
includes += wl_includes

wl_dep = dependency('wayland-client')

subdir('protocol')

wayland_module = shared_module(
    'clippor-wayland',
    wl_sources,
    include_directories: wl_includes,
    dependencies: [wl_dep, glib_dep, gio_unix_dep],
    link_with: wl_link_with,
    install: false,
    override_options: ['strip=false', 'debug=true']
    )

# Used by tests
wayland_library = shared_library(
    'clippor-wayland-lib',
    wl_sources,
    include_directories: wl_includes,
    dependencies: [wl_dep, glib_dep, gio_unix_dep],
    link_with: wl_link_with,
    override_options: ['b_lundef=false']
    )
