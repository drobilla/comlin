# Copyright 2020-2023 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: BSD-2-Clause

run_test_py = files('run_test.py')

# Unit Tests

test(
  'history',
  executable(
    'test_history',
    files('test_history.c'),
    c_args: c_suppressions,
    dependencies: comlin_dep,
    include_directories: include_dirs,
  ),
)


# Data-Driven Tests

test_comlin = executable(
  'test_comlin',
  files('test_comlin.c'),
  c_args: c_suppressions,
  dependencies: comlin_dep,
  include_directories: include_dirs,
)

subdir('common')
subdir('dumb')
subdir('history')
subdir('mask')
subdir('multi')
subdir('single')
