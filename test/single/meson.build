# Copyright 2020-2023 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: BSD-2-Clause

single_test_names = [
  'Backspace',
  'Ca',
  'CaCa',
  'CaCe',
  'Cb',
  'CbCbCt',
  'CbCd',
  'CbCf',
  'CbCk',
  'CbCt',
  'CbCu',
  'CbCw',
  'Ch',
  'Ck',
  'Cl',
  'CpCp',
  'CpCpCn',
  'CpCpCnCn',
  'CpCpCp',
  'Cu',
  'Cw',
  'CwCw',
  'Home',
  'Home2',
  'Home2End2',
  'HomeEnd',
  'Left',
  'Tab',
  'UpUp',
  'UpUpDown',
  'fiTab',
  'fiTabEsc',
  'fiTabTab',
  'fiTabTabTab',
  'fiTabiTab',
  'middle',
  'seTab',
  'seTabTab',
]

foreach name : single_test_names
  in_file = files(name + '.in.ans')
  out_file = files(name + '.out.ans')

  test(
    name,
    run_test_py,
    args: [out_file, test_comlin, in_file],
    suite: ['io', 'single'],
  )
endforeach
