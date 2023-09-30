// Copyright 2023 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: BSD-2-Clause

#undef NDEBUG

#include "comlin/comlin.h"

#include <assert.h>

static int const ifd = 0; // stdin
static int const ofd = 1; // stdout

static void
test_empty(void)
{
    ComlinState* const state = comlin_new_state(ifd, ofd, "> ", 0U);
    assert(state);
    assert(!comlin_history_add(state, "one"));
    comlin_free_state(state);
}

static void
test_bad_load(void)
{
    ComlinState* const state = comlin_new_state(ifd, ofd, "> ", 8U);
    assert(state);
    assert(!comlin_history_add(state, "one"));
    assert(comlin_history_load(state, "/does/not/exist") == COMLIN_NO_FILE);
    comlin_free_state(state);
}

static void
test_bad_save(void)
{
    ComlinState* const state = comlin_new_state(ifd, ofd, "> ", 8U);
    assert(state);
    assert(!comlin_history_add(state, "one"));
    assert(comlin_history_save(state, "/does/not/exist") == COMLIN_NO_FILE);
    comlin_free_state(state);
}

int
main(void)
{
    test_empty();
    test_bad_load();
    test_bad_save();
    return 0;
}
