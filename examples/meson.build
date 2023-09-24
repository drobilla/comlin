# Copyright 2020-2023 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: BSD-2-Clause

example = executable(
  'example',
  files('example.c'),
  c_args: c_suppressions,
  dependencies: linenoise_dep,
  include_directories: include_dirs,
)

test('badarg', example, args: ['--badarg'], should_fail: true)
