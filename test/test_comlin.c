// Copyright 2023 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: BSD-2-Clause

#include "comlin/comlin.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char const* restore_path;
    char const* save_path;
    bool dumb;
    bool mask;
    bool multiline;
} Options;

static bool
starts_with(char const* const string, char const* const prefix)
{
    return !strncmp(string, prefix, strlen(string));
}

static void
completion(char const* const line, ComlinCompletions* const lc)
{
    if (starts_with(line, "first")) {
        comlin_add_completion(lc, "first");
        comlin_add_completion(lc, "firstish");
    } else if (starts_with(line, "second")) {
        comlin_add_completion(lc, "second");
        comlin_add_completion(lc, "secondish");
    }
}

static int
print_usage(char const* const name, bool const error)
{
    static char const* const description =
      "Run an input/output test.\n"
      "INPUT is read directly and may contain terminal escapes.\n"
      "Output is written to stdout.\n\n"
      "  --dumb          Force dumb terminal mode.\n"
      "  --help          Display this help and exit.\n"
      "  --mask          Use mask mode.\n"
      "  --multi         Use multi-line mode.\n"
      "  --restore FILE  Load history from FILE before run.\n"
      "  --save FILE     Save history to FILE after run.\n";

    FILE* const os = error ? stderr : stdout;
    fprintf(os, "%s", error ? "\n" : "");
    fprintf(os, "Usage: %s [OPTION]... [INPUT]\n", name);
    fprintf(os, "%s", description);
    return error ? 1 : 0;
}

static int
missing_arg(char const* const name, char const* const opt)
{
    fprintf(stderr, "%s: option '%s' requires an argument\n", name, opt);
    return print_usage(name, true);
}

static int
run(int const ifd, int const ofd, Options const opts)
{
    bool const mask = opts.mask;
    bool const multiline = opts.multiline;
    char const* const restore_path = opts.restore_path;
    char const* const save_path = opts.save_path;

    // Allocate and configure state
    char const* const term = opts.dumb ? "dumb" : "vt100";
    ComlinState* const state = comlin_new_state(ifd, ofd, term, 32U);
    comlin_set_completion_callback(state, completion);
    comlin_set_mode(state,
                    (mask ? COMLIN_MODE_MASKED : 0U) |
                      (multiline ? COMLIN_MODE_MULTI_LINE : 0U));

    // Load initial history
    if (restore_path) {
        if (comlin_history_load(state, restore_path)) {
            fprintf(stderr, "Failed to load history file '%s'\n", restore_path);
            return 1;
        }
    }

    // Process input lines until end of input or an error
    ComlinStatus st = COMLIN_SUCCESS;
    while (!st) {
        st = comlin_read_line(state, "> ");
        if (!st) {
            char const* const line = comlin_text(state);
            printf("echo: %s\n", line);
            fflush(stdout);
            comlin_history_add(state, line);
        }
    }

    // Save updated history
    if (save_path) {
        if (comlin_history_save(state, save_path)) {
            fprintf(stderr, "Failed to save history file '%s'\n", save_path);
            return 1;
        }
    }

    comlin_free_state(state);
    return st == COMLIN_END ? 0 : 1;
}

int
main(int const argc, char const* const* const argv)
{
    // Parse command line options
    Options opts = {NULL, NULL, false, false, false};
    int a = 1;
    for (; a < argc && argv[a][0] == '-'; ++a) {
        if (!strcmp(argv[a], "--help")) {
            return print_usage(argv[0], false);
        }

        if (!strcmp(argv[a], "--dumb")) {
            opts.dumb = true;
        } else if (!strcmp(argv[a], "--mask")) {
            opts.mask = true;
        } else if (!strcmp(argv[a], "--multi")) {
            opts.multiline = true;
        } else if (!strcmp(argv[a], "--restore")) {
            if (++a == argc) {
                return missing_arg(argv[0], "--restore");
            }

            opts.restore_path = argv[a];
        } else if (!strcmp(argv[a], "--save")) {
            if (++a == argc) {
                return missing_arg(argv[0], "--save");
            }

            opts.save_path = argv[a];
        } else {
            return print_usage(argv[0], true);
        }
    }

    return (a < argc - 1) ? print_usage(argv[0], true) : run(0, 1, opts);
}
