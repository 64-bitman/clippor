wayland_scanner = find_program('wayland-scanner', required: true, native: true)

protocols = ['ext-data-control-v1', 'wlr-data-control-unstable-v1']

foreach name : protocols
    header = custom_target(name + ' client header',
        input: name + '.xml', output: name + '.h',
        command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@']
    )
    code = custom_target(name + ' code',
        input: name + '.xml', output: name + '.c',
        command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@']
    )

    includes += include_directories('.')
    link_with += static_library(name, code, header, dependencies: wl_dep)
endforeach
