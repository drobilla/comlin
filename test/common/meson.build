# Copyright 2020-2023 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: BSD-2-Clause

common_test_names = [
  'CbDelete',
  'Cd',
  'Ce',
  'Ck',
  'Cn',
  'Ct',
  'LeftRight',
  'one',
  'two',
]

foreach name : common_test_names
  in_file = files(name + '.in.ans')
  out_file = files(name + '.out.ans')

  test(
    name + '_single',
    run_test_py,
    args: [out_file, test_comlin, in_file],
    suite: ['io', 'common'],
  )

  test(
    name + '_multi',
    run_test_py,
    args: [out_file, '--', test_comlin, '--multi', in_file],
    suite: ['io', 'common'],
  )
endforeach
