# Copyright 2020-2026 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: BSD-2-Clause

# Lint

if get_option('lint')
  all_sources = c_headers + sources + example_sources

  # Check code formatting
  clang_format = find_program('clang-format', required: false)
  if clang_format.found()
    test(
      'format',
      clang_format,
      args: ['--Werror', '--dry-run'] + all_sources,
      suite: 'code',
    )
  endif

  if not meson.is_subproject()
    # Check licensing metadata
    reuse = find_program('reuse', required: false)
    if reuse.found()
      test(
        'REUSE',
        reuse,
        args: ['--root', linenoise_src_root, 'lint'],
        suite: 'data',
      )
    endif
  endif
endif
