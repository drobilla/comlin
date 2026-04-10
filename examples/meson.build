# Copyright 2020-2026 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: BSD-2-Clause

example_sources = files('example.c')

example = executable(
  'example',
  example_sources,
  c_args: c_suppressions,
  dependencies: comlin_dep,
  implicit_include_directories: false,
  include_directories: include_dirs,
)

test('badarg', example, args: ['--badarg'], should_fail: true)
