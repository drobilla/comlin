# Copyright 2020-2023 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: BSD-2-Clause

project(
  'linenoise',
  ['c'],
  default_options: [
    'b_ndebug=if-release',
    'buildtype=release',
    'c_std=c99',
  ],
  license: 'ISC',
  meson_version: '>= 0.54.0',
  version: '0.0.1',
)

linenoise_src_root = meson.current_source_dir()
linenoise_build_root = meson.current_build_dir()
major_version = meson.project_version().split('.')[0]
version_suffix = '-@0@'.format(major_version)
versioned_name = 'linenoise' + version_suffix

#######################
# Compilers and Flags #
#######################

# Required tools
pkg = import('pkgconfig')
cc = meson.get_compiler('c')

# Set global warning suppressions
c_suppressions = []
warning_level = get_option('warning_level')
if cc.get_id() == 'clang'
  if warning_level == 'everything'
    c_suppressions += [
      '-Wno-declaration-after-statement',
      '-Wno-padded',
      '-Wno-shorten-64-to-32',
      '-Wno-sign-conversion',
      '-Wno-unsafe-buffer-usage',
    ]

    if not meson.is_cross_build()
      c_suppressions += ['-Wno-poison-system-directories']
    endif
  endif

elif cc.get_id() == 'gcc'
  if warning_level == 'everything'
    c_suppressions += [
      '-Wno-conversion',
      '-Wno-discarded-qualifiers',
      '-Wno-padded',
      '-Wno-strict-overflow',
      '-Wno-suggest-attribute=pure',
      '-Wno-switch-default',
    ]
  endif
endif

c_suppressions = cc.get_supported_arguments(c_suppressions)

##########################
# Platform Configuration #
##########################

# Use versioned name everywhere to support parallel major version installations
if host_machine.system() == 'windows'
  if get_option('default_library') == 'both'
    error('default_library=both is not supported on Windows')
  endif
  soversion = ''
else
  soversion = meson.project_version().split('.')[0]
endif

###########
# Library #
###########

include_dirs = include_directories(['include'])
c_headers = files('include/linenoise/linenoise.h')
sources = files('src/linenoise.c')

# Set appropriate arguments for building against the library type
extra_c_args = ['-D_POSIX_C_SOURCE=200809L']
if get_option('default_library') == 'static'
  extra_c_args = ['-DLINENOISE_STATIC']
endif

# Build shared and/or static library
liblinenoise = library(
  versioned_name,
  sources,
  c_args: c_suppressions + extra_c_args + ['-DLINENOISE_INTERNAL'],
  # gnu_symbol_visibility: 'hidden',
  include_directories: include_dirs,
  install: true,
  soversion: soversion,
  version: meson.project_version(),
)

# Declare dependency for internal meson dependants
linenoise_dep = declare_dependency(
  compile_args: extra_c_args,
  include_directories: include_dirs,
  link_with: liblinenoise,
)

# Generage pkg-config file for external dependants
pkg.generate(
  liblinenoise,
  description: 'A minimal command line editing library',
  extra_cflags: extra_c_args,
  filebase: versioned_name,
  name: 'Linenoise',
  subdirs: [versioned_name],
  version: meson.project_version(),
)

# Override pkg-config dependency for internal meson dependants
meson.override_dependency(versioned_name, linenoise_dep)

# Install header to a versioned include directory
install_headers(c_headers, subdir: versioned_name / 'linenoise')

#########
# Tests #
#########

if not meson.is_subproject()
  # Check licensing metadata
  reuse = find_program('reuse', required: false)
  if reuse.found()
    test(
      'REUSE',
      reuse,
      args: ['--root', linenoise_src_root, 'lint'],
      suite: 'data',
    )
  endif
endif

############
# Examples #
############

subdir('examples')

# Display configuration summary
if not meson.is_subproject()
  summary('Install prefix', get_option('prefix'))
  summary('Headers', get_option('prefix') / get_option('includedir'))
  summary('Libraries', get_option('prefix') / get_option('libdir'))
endif
