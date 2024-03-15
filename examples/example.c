// Copyright 2010-2023 Salvatore Sanfilippo <antirez@gmail.com>
// Copyright 2010-2013 Pieter Noordhuis <pcnoordhuis@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause

#include "comlin/comlin.h"

#include <sys/select.h>
#include <unistd.h>

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
printString(char const* const str)
{
    write(1, str, strlen(str));
}

int
main(int argc, char** argv)
{
    char* const prgname = argv[0];

    char const* line = NULL;
    int async = 0;
    int multiline = 0;

    // Parse options, with --multiline we enable multi line editing
    while (argc > 1) {
        --argc;
        ++argv;
        if (!strcmp(*argv, "--multiline")) {
            multiline = 1;
            printString("Multi-line mode enabled.\n");
        } else if (!strcmp(*argv, "--async")) {
            async = 1;
        } else {
            printString("Usage: ");
            printString(prgname);
            printString(" [--multiline] [--keycodes] [--async]\n");
            return 1;
        }
    }

    ComlinState* const state = comlin_new_state(0, 1, getenv("TERM"), 100U);
    if (multiline) {
        comlin_set_mode(state, COMLIN_MODE_MULTI_LINE);
    }

    /* Set the completion callback. This will be called every time the
     * user uses the <tab> key. */
    comlin_set_completion_callback(state, completion);

    /* Load history from file. The history file is just a plain text file
     * where entries are separated by newlines. */
    comlin_history_load(state, "history.txt"); // Load the history at startup

    /* Now this is the main loop of the typical comlin-based application.
     * The call to comlin() will block as long as the user types something
     * and presses enter.
     *
     * The typed string is returned as a malloc() allocated string by
     * comlin, so the user needs to free() it. */

    while (1) {
        if (!async) {
            ComlinStatus const st = comlin_read_line(state, "hello> ");
            if (!st) {
                line = comlin_text(state);
            } else {
                line = NULL;
                break;
            }
        } else {
            /* Asynchronous mode using the multiplexing API: wait for
             * data on stdin, and simulate async data coming from some source
             * using the select(2) timeout. */
            comlin_edit_start(state, "hello> ");
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
                    return 1;
                }

                if (retval) {
                    const ComlinStatus st = comlin_edit_feed(state);
                    if (st == COMLIN_INTERRUPTED || st == COMLIN_END) {
                        line = NULL;
                        break;
                    }

                    if (!st) {
                        line = comlin_text(state);
                        break;
                    }
                } else {
                    // Timeout occurred
                    static int counter = 0;
                    comlin_hide(state);
                    printString("Async output ");
                    char decimal[24] = {0};
                    (void)snprintf(decimal, sizeof(decimal), "%d\n", counter++);
                    printString(decimal);
                    comlin_show(state);
                }
            }
            comlin_edit_stop(state);
            if (!line) { // Ctrl+D/C
                comlin_free_state(state);
                return 0;
            }
        }

        // Do something with the string
        if (line[0] != '\0' && line[0] != '/') {
            printString("echo: '");
            printString(line);
            printString("'\n");
            comlin_history_add(state, line);           // Add to the history
            comlin_history_save(state, "history.txt"); // Save history to disk
        } else if (!strncmp(line, "/mask", 5)) {
            comlin_set_mode(
              state,
              (ComlinModeFlags)COMLIN_MODE_MASKED |
                (multiline ? (ComlinModeFlags)COMLIN_MODE_MULTI_LINE : 0U));
        } else if (!strncmp(line, "/unmask", 7)) {
            comlin_set_mode(state, (multiline ? COMLIN_MODE_MULTI_LINE : 0U));
        } else if (line[0] == '/') {
            printString("Unreconized command: ");
            printString(line);
            printString("\n");
        }
    }

    comlin_free_state(state);
    return 0;
}
