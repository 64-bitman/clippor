sources += files('clippor-entry.c', 'clippor-clipboard.c', 'clippor-client.c', 'database.c', 'dbus-service.c', 'util.c', 'config.c', 'server.c', 'tomlc17.c')
includes += include_directories('include')

gnome = import('gnome')

sources += gnome.gdbus_codegen('com.github.clippor',
  sources: 'com.github.clippor.xml',
  interface_prefix: 'com.github.',
  namespace: 'Bus',
  object_manager: true,
  extra_args: ['--glib-max-allowed', '2.74', '--pragma-once'],
  )

subdir('wayland')
