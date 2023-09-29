# Copyright 2020-2023 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: BSD-2-Clause

common_test_names = [
  'Backspace',
  'Ca',
  'CaCa',
  'CaCe',
  'CaCt',
  'Cb',
  'CbCbCt',
  'CbCd',
  'CbCf',
  'CbCk',
  'CbCt',
  'CbCu',
  'CbCw',
  'CbDelete',
  'Cd',
  'Ce',
  'Ch',
  'Ck',
  'Cl',
  'Cn',
  'CpCp',
  'CpCpCn',
  'CpCpCnCn',
  'CpCpCp',
  'Ct',
  'Cu',
  'Cw',
  'CwCw',
  'Home',
  'Home2',
  'Home2End2',
  'HomeEnd',
  'Left',
  'LeftRight',
  'Tab',
  'UpUp',
  'UpUpDown',
  'fiTab',
  'fiTabEsc',
  'fiTabTab',
  'fiTabTabTab',
  'fiTabiTab',
  'one',
  'seTab',
  'seTabTab',
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
