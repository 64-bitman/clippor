project('clippor', 'c',
  version: 'v0.0.1',
  license: 'MIT',
  license_files: 'LICENSE',
  default_options: ['c_std=gnu11', 'warning_level=2'],
  meson_version: '>= 1.1.0'
  )

if get_option('buildtype').startswith('debug')
  add_project_arguments('-DDEBUG',
    '-ggdb3',
    # '-pg',
    # '-no-pie',
    language : 'c'
    )
  # add_project_link_arguments(['-pg', '-no-pie'], language: 'c')
endif

add_project_arguments(
  '-DG_LOG_USE_STRUCTURED',
  '-DVERSION="' + meson.project_version() + '"',
  language: 'c'
  )

glib_dep = dependency('glib-2.0')
gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
gmodule_dep = dependency('gmodule-2.0')
sqlite_dep = dependency('sqlite3')

deps = [
  glib_dep,
  gio_dep,
  gio_unix_dep,
  gmodule_dep,
  sqlite_dep
  ]

sources = []
includes = []
link_with = []

subdir('src')

clippor = executable(
  'clippor',
  'src/main.c',
  sources,
  include_directories: includes,
  dependencies: deps,
  link_with: link_with,
  export_dynamic: true
  )
