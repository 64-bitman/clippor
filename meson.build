project('clippor', 'c',
  version: 'v0.0.1',
  license: 'MIT',
  license_files: 'LICENSE',
  default_options: ['c_std=gnu11', 'warning_level=3'],
  meson_version: '>= 1.1.0'
  )

if get_option('buildtype').startswith('debug')
  add_project_arguments('-DDEBUG',
    # '-pg',
    '-DG_LOG_USE_STRUCTURED',
    '-DVERSION="' + meson.project_version() + '"',
    language : 'c')
  # add_project_link_arguments('-pg', language: 'c')
endif

env = environment()

env.set('GSETTINGS_SCHEMA_DIR', 'schemas/')

deps = []
link_with = []
sources = []
includes = []

test_sources = []
test_includes = []

glib_dep = dependency('glib-2.0')
gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
sqlite_dep = dependency('sqlite3')
wl_dep = dependency('wayland-client', required: get_option('wayland'))
x11_dep = dependency('xcb', required: get_option('x11'))

deps += [glib_dep, gio_dep, gio_unix_dep, wl_dep, x11_dep, sqlite_dep]

cc = meson.get_compiler('c')
cfg_data = configuration_data()

if wl_dep.found()
  cfg_data.set('FEAT_WAYLAND', true)
endif

if x11_dep.found()
    cfg_data.set('FEAT_X11', true)
endif

configure_file(output: 'project_config.h', configuration: cfg_data)
includes += include_directories('.')

subdir('tests')
subdir('schemas')
subdir('src')

sources += 'src/main.c'

executable('clippor',
  sources,
  include_directories: includes,
  dependencies: deps,
  link_with: link_with
  )
