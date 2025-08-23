gnome = import('gnome')

dbus_src = gnome.gdbus_codegen(
    'com.github.Clippor',
    sources: 'com-github-clippor.xml',
    interface_prefix: 'com.github',
    namespace: 'DBus',
    object_manager: true,
    extra_args: ['--glib-min-required', '2.74']
)

sources += dbus_src
