// Copyright 2023 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: BSD-2-Clause

#include "comlin/comlin.h"

#include <unistd.h>

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

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

static void
print_string(char const* const str)
{
    write(1, str, strlen(str));
}

static int
print_usage(char const* const name, bool const error)
{
    static char const* const description =
      "Run an input/output test.\n"
      "INPUT is read directly and may contain terminal escapes.\n"
      "Output is written to stdout.\n"
      "  --mask   Use mask mode.\n"
      "  --multi  Use multi-line mode.\n";

    FILE* const os = error ? stderr : stdout;
    fprintf(os, "%s", error ? "\n" : "");
    fprintf(os, "Usage: %s [OPTION]... [INPUT]\n", name);
    fprintf(os, "%s", description);
    return error ? 1 : 0;
}

int
main(int const argc, char const* const* const argv)
{
    // Parse command line options
    bool mask = false;
    bool multiline = false;
    int a = 1;
    for (; a < argc && argv[a][0] == '-'; ++a) {
        if (!strcmp(argv[a], "--mask")) {
            mask = true;
        } else if (!strcmp(argv[a], "--multi")) {
            multiline = true;
        } else {
            return print_usage(argv[0], true);
        }
    }

    if (a < argc - 1) {
        return print_usage(argv[0], true);
    }

    // Open input file if given, otherwise use stdin
    int const ofd = STDOUT_FILENO;
    int ifd = STDIN_FILENO;
    if (a < argc) {
        ifd = open(argv[a], O_RDONLY | O_CLOEXEC);
        if (ifd < 0) {
            fprintf(stderr, "Failed to open input file '%s'\n", argv[a]);
            return 1;
        }
    }

    // Allocate and configure state
    ComlinState* const state = comlin_new_state(ifd, ofd, "vt100", 64U);
    comlin_set_completion_callback(state, completion);
    if (mask) {
        comlin_mask_mode_enable(state);
    }
    if (multiline) {
        comlin_set_multi_line(state, 1);
    }

    // Process input lines until end of input or an error
    ComlinStatus st = COMLIN_SUCCESS;
    while (!st) {
        st = comlin_read_line(state, "> ");
        if (!st) {
            char const* const line = comlin_text(state);
            print_string("echo: ");
            print_string(line);
            print_string("\n");
            comlin_history_add(state, line);
        }
    }

    comlin_free_state(state);
    return st == COMLIN_END ? 0 : 1;
}
