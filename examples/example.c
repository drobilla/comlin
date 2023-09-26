// Copyright 2010-2023 Salvatore Sanfilippo <antirez@gmail.com>
// Copyright 2010-2013 Pieter Noordhuis <pcnoordhuis@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause

#include "comlin/comlin.h"

#include <sys/select.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void completion(const char *buf, comlinCompletions *lc) {
    if (buf[0] == 'h') {
        comlinAddCompletion(lc, "hello");
        comlinAddCompletion(lc, "hello there");
    }
}

static char *hints(const char *buf, int *color, int *bold) {
    if (!strcasecmp(buf, "hello")) {
        *color = 35;
        *bold = 0;
        return " World";
    }
    return NULL;
}

int main(int argc, char **argv) {
    char *line = NULL;
    char *prgname = argv[0];
    int async = 0;

    // Parse options, with --multiline we enable multi line editing
    while (argc > 1) {
        --argc;
        ++argv;
        if (!strcmp(*argv, "--multiline")) {
            comlinSetMultiLine(1);
            printf("Multi-line mode enabled.\n");
        } else if (!strcmp(*argv, "--keycodes")) {
            comlinPrintKeyCodes();
            exit(0);
        } else if (!strcmp(*argv, "--async")) {
            async = 1;
        } else {
            fprintf(stderr,
                    "Usage: %s [--multiline] [--keycodes] [--async]\n",
                    prgname);
            exit(1);
        }
    }

    /* Set the completion callback. This will be called every time the
     * user uses the <tab> key. */
    comlinSetCompletionCallback(completion);
    comlinSetHintsCallback(hints);

    /* Load history from file. The history file is just a plain text file
     * where entries are separated by newlines. */
    comlinHistoryLoad("history.txt"); // Load the history at startup

    /* Now this is the main loop of the typical comlin-based application.
     * The call to comlin() will block as long as the user types something
     * and presses enter.
     *
     * The typed string is returned as a malloc() allocated string by
     * comlin, so the user needs to free() it. */

    while (1) {
        if (!async) {
            line = comlin("hello> ");
            if (line == NULL) {
                break;
            }
        } else {
            /* Asynchronous mode using the multiplexing API: wait for
             * data on stdin, and simulate async data coming from some source
             * using the select(2) timeout. */
            struct comlinState ls;
            char buf[1024];
            comlinEditStart(&ls, -1, -1, buf, sizeof(buf), "hello> ");
            while (1) {
                fd_set readfds;
                struct timeval tv;

                FD_ZERO(&readfds);
                FD_SET(ls.ifd, &readfds);
                tv.tv_sec = 1; // 1 sec timeout
                tv.tv_usec = 0;

                int retval = select(ls.ifd + 1, &readfds, NULL, NULL, &tv);
                if (retval == -1) {
                    perror("select()");
                    exit(1);
                } else if (retval) {
                    line = comlinEditFeed(&ls);
                    /* A NULL return means: line editing is continuing.
                     * Otherwise the user hit enter or stopped editing
                     * (CTRL+C/D). */
                    if (line != comlinEditMore) {
                        break;
                    }
                } else {
                    // Timeout occurred
                    static int counter = 0;
                    comlinHide(&ls);
                    printf("Async output %d.\n", counter++);
                    comlinShow(&ls);
                }
            }
            comlinEditStop(&ls);
            if (line == NULL) { // Ctrl+D/C
                exit(0);
            }
        }

        // Do something with the string
        if (line[0] != '\0' && line[0] != '/') {
            printf("echo: '%s'\n", line);
            comlinHistoryAdd(line);           // Add to the history
            comlinHistorySave("history.txt"); // Save the history on disk
        } else if (!strncmp(line, "/historylen", 11)) {
            // The "/historylen" command will change the history len
            int len = atoi(line + 11);
            comlinHistorySetMaxLen(len);
        } else if (!strncmp(line, "/mask", 5)) {
            comlinMaskModeEnable();
        } else if (!strncmp(line, "/unmask", 7)) {
            comlinMaskModeDisable();
        } else if (line[0] == '/') {
            printf("Unreconized command: %s\n", line);
        }
        free(line);
    }
    return 0;
}
