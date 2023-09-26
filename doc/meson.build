# Copyright 2021-2023 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: BSD-2-Clause

# Find required programs
doxygen = find_program('doxygen', required: get_option('docs'))
build_docs = doxygen.found()
if build_docs
  # Configure Doxyfile for Doxygen
  config = configuration_data()
  config.set('COMLIN_SRCDIR', comlin_src_root)
  config.set('DOX_OUTPUT', meson.current_build_dir())
  doxyfile = configure_file(
    configuration: config,
    input: files('Doxyfile.in'),
    output: 'Doxyfile',
  )

  # Run Doxygen to generate "html" subdirectory
  doxygen_xml = custom_target(
    'html',
    build_by_default: true,
    command: [doxygen, '@INPUT0@'],
    input: [doxyfile] + c_headers,
    output: 'html',
  )
endif
