# Copyright 2020-2026 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: BSD-2-Clause

# Lint

if get_option('lint')
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
