# Copyright 2020-2023 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: BSD-2-Clause

dumb_test_names = [
  'Cc',
  'Cd',
  'one',
]

foreach name : dumb_test_names
  in_file = files(name + '.in.ans')
  out_file = files(name + '.out.ans')

  test(
    name,
    run_test_py,
    args: [in_file, out_file, test_comlin, '--dumb'],
    suite: ['io', 'dumb'],
  )
endforeach
