# Copyright 2020-2023 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: BSD-2-Clause

run_test_py = files('run_test.py')

test_comlin = executable(
  'test_comlin',
  files('test_comlin.c'),
  c_args: c_suppressions,
  dependencies: comlin_dep,
  include_directories: include_dirs,
)

subdir('common')
subdir('mask')
subdir('single')
subdir('multi')
