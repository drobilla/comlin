// Copyright 2023-2026 David Robillard <d@drobilla.net>
// Copyright 2010-2023 Salvatore Sanfilippo <antirez@gmail.com>
// Copyright 2010-2013 Pieter Noordhuis <pcnoordhuis@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause

#include "comlin/comlin.h"

#ifdef _WIN32
#    include <io.h>
#else
#    include <sys/select.h>
#    include <unistd.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
completion(char const* buf, ComlinCompletions* const lc)
{
    if (buf[0] == 'h') {
        comlin_add_completion(lc, "hello");
        comlin_add_completion(lc, "hello there");
    }
}

static void
print_string(char const* const str)
{
    write(1, str, strlen(str));
}

static char const*
read_line_sync(ComlinState* const state)
{
    ComlinStatus const st = comlin_read_line(state, "example> ");

    return !st ? comlin_text(state) : NULL;
}

#ifndef _WIN32

static char const*
read_line_async(ComlinState* const state)
{
    comlin_edit_start(state, "example> ");

    while (1) {
        fd_set readfds;
        struct timeval tv;

        FD_ZERO(&readfds);
        FD_SET(0, &readfds);
        tv.tv_sec = 1; // 1 sec timeout
        tv.tv_usec = 0;

        int const retval = select(1, &readfds, NULL, NULL, &tv);
        if (retval == -1) {
            perror("select()");
            break;
        }

        if (retval) {
            ComlinStatus const st = comlin_edit_feed(state);
            if (st == COMLIN_INTERRUPTED || st == COMLIN_END) {
                break;
            }

            if (!st) {
                comlin_edit_stop(state);
                return comlin_text(state);
            }
        } else {
            // Timeout occurred
            static int counter = 0;
            comlin_hide(state);
            print_string("Async output ");
            char decimal[24] = {0};
            (void)snprintf(decimal, sizeof(decimal), "%d\n", counter++);
            print_string(decimal);
            comlin_show(state);
        }
    }

    comlin_edit_stop(state);
    return NULL;
}

#endif

static void
process_line(ComlinState* const state, char const* line)
{
    if (line[0] != '\0' && line[0] != '/') {
        print_string("echo: ");
        print_string(line);
        print_string("\n");
        comlin_history_add(state, line);
        comlin_history_save(state, "history.txt");
    } else if (!strncmp(line, "/mask", 5)) {
        comlin_set_mode(state, (ComlinModeFlags)COMLIN_MODE_MASKED);
    } else if (!strncmp(line, "/unmask", 7)) {
        comlin_set_mode(state, 0U);
    } else if (line[0] == '/') {
        print_string("Unrecognized command: ");
        print_string(line);
        print_string("\n");
    }
}

int
main(int const argc, char** const argv)
{
    // Parse options
    int async = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--async")) {
            async = 1;
        } else {
            print_string("Usage: ");
            print_string(argv[0]);
            print_string(" [--keycodes] [--async]\n");
            return 1;
        }
    }

    // Set up comlin and load history
    ComlinState* const state = comlin_new_state(0, 1, getenv("TERM"), 100U);
    comlin_set_completion_callback(state, completion);
    comlin_history_load(state, "history.txt");

    // Read and process lines until interrupt or error
    char const* line = "";
    while (line) {
        if (async) {
#ifndef _WIN32
            line = read_line_async(state);
#endif
        } else {
            line = read_line_sync(state);
        }

        if (line) {
            process_line(state, line);
        }
    }

    comlin_free_state(state);
    return 0;
}
