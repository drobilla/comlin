# Copyright 2020-2023 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: BSD-2-Clause

history_test_names = [
  'Up',
  'four',
  'many',
  'three',
]

restore_file = files('start.hist.txt')

foreach name : history_test_names
  in_file = files(name + '.in.ans')
  out_file = files(name + '.out.ans')

  test(
    name,
    run_test_py,
    args: [
      ['--history', files(name + '.hist.txt')],
      in_file,
      out_file,
      test_comlin,
      ['--restore', restore_file],
    ],
    suite: ['io', 'history'],
  )
endforeach
