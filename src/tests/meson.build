env =  [
  'G_TEST_SRCDIR=@0@'.format(meson.current_source_dir()),
  'G_TEST_BUILDDIR=@0@'.format(meson.current_build_dir()),
  ]
protocol = 'tap'

lib = static_library(name, sources,
  include_directories: includes,
  dependencies: deps,
  link_with: link_with
  )

# test(
#   'clippor-entry',
#   executable(
#     'clippor-entry',
#     'clippor-entry.c',
#     dependencies: deps,
#     include_directories: includes,
#     link_with: lib),
#   env: env,
#   protocol: protocol
#   )
