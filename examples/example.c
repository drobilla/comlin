// Copyright 2010-2023 Salvatore Sanfilippo <antirez@gmail.com>
// Copyright 2010-2013 Pieter Noordhuis <pcnoordhuis@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause

#include "comlin/comlin.h"

#include <sys/select.h>
#include <unistd.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
completion(const char* buf, ComlinCompletions* lc)
{
    if (buf[0] == 'h') {
        comlinAddCompletion(lc, "hello");
        comlinAddCompletion(lc, "hello there");
    }
}

static void
printString(const char* const str)
{
    write(1, str, strlen(str));
}

static void
printKeyCodesLoop(void)
{
    char buf[1024];
    char quit[4] = {' ', ' ', ' ', ' '};

    fprintf(
      stderr,
      "Press keys to see scan codes.  Type 'quit' at any time to exit.\n");

    // Start an edit just to set the terminal to raw mode
    ComlinState* const state = comlinNewState(0, 1, buf, sizeof(buf), "");
    comlinEditStart(state);

    // Ignore it and process input keys ourselves
    while (1) {
        // Read an input character
        char c = '\0';
        const int nread = read(STDIN_FILENO, &c, 1);
        if (nread <= 0) {
            continue;
        }

        // Insert it as the rightmost in the buffer (pushing one off the left)
        memmove(quit, quit + 1, sizeof(quit) - 1);
        quit[sizeof(quit) - 1] = c;
        if (memcmp(quit, "quit", sizeof(quit)) == 0) {
            break;
        }

        // Print the key code and continue
        fprintf(stderr,
                "'%c' %02x (%d) (type quit to exit)\n\n",
                isprint(c) ? c : '?',
                (unsigned)c,
                (int)c);
    }

    // Reset terminal mode
    comlinEditStop(state);
    comlinFreeState(state);
}

int
main(int argc, char** argv)
{
    char* line = NULL;
    char* prgname = argv[0];
    int async = 0;
    int multiline = 0;

    char buf[1024];

    // Parse options, with --multiline we enable multi line editing
    while (argc > 1) {
        --argc;
        ++argv;
        if (!strcmp(*argv, "--multiline")) {
            multiline = 1;
            printString("Multi-line mode enabled.\n");
        } else if (!strcmp(*argv, "--keycodes")) {
            printKeyCodesLoop();
            return 0;
        } else if (!strcmp(*argv, "--async")) {
            async = 1;
        } else {
            printString("Usage: ");
            printString(prgname);
            printString(" [--multiline] [--keycodes] [--async]\n");
            return 1;
        }
    }

    ComlinState* const state = comlinNewState(0, 1, buf, sizeof(buf), "hello>");
    if (multiline) {
        comlinSetMultiLine(state, 1);
    }

    /* Set the completion callback. This will be called every time the
     * user uses the <tab> key. */
    comlinSetCompletionCallback(state, completion);

    /* Load history from file. The history file is just a plain text file
     * where entries are separated by newlines. */
    comlinHistoryLoad(state, "history.txt"); // Load the history at startup

    /* Now this is the main loop of the typical comlin-based application.
     * The call to comlin() will block as long as the user types something
     * and presses enter.
     *
     * The typed string is returned as a malloc() allocated string by
     * comlin, so the user needs to free() it. */

    while (1) {
        if (!async) {
            line = comlinReadLine(state, "hello> ");
            if (line == NULL) {
                break;
            }
        } else {
            /* Asynchronous mode using the multiplexing API: wait for
             * data on stdin, and simulate async data coming from some source
             * using the select(2) timeout. */
            comlinEditStart(state);
            while (1) {
                fd_set readfds;
                struct timeval tv;

                FD_ZERO(&readfds);
                FD_SET(0, &readfds);
                tv.tv_sec = 1; // 1 sec timeout
                tv.tv_usec = 0;

                int retval = select(1, &readfds, NULL, NULL, &tv);
                if (retval == -1) {
                    perror("select()");
                    return 1;
                }

                if (retval) {
                    line = comlinEditFeed(state);
                    /* A NULL return means: line editing is continuing.
                     * Otherwise the user hit enter or stopped editing
                     * (CTRL+C/D). */
                    if (line != comlinEditMore) {
                        break;
                    }
                } else {
                    // Timeout occurred
                    static int counter = 0;
                    comlinHide(state);
                    printString("Async output ");
                    char decimal[24] = {0};
                    snprintf(decimal, sizeof(decimal), "%d\n", counter++);
                    printString(decimal);
                    comlinShow(state);
                }
            }
            comlinEditStop(state);
            if (line == NULL) { // Ctrl+D/C
                comlinFreeState(state);
                return 0;
            }
        }

        // Do something with the string
        if (line[0] != '\0' && line[0] != '/') {
            printString("echo: '");
            printString(line);
            printString("'\n");
            comlinHistoryAdd(state, line);           // Add to the history
            comlinHistorySave(state, "history.txt"); // Save history to disk
        } else if (!strncmp(line, "/historylen", 11)) {
            // The "/historylen" command will change the history len
            int len = atoi(line + 11);
            comlinHistorySetMaxLen(state, len);
        } else if (!strncmp(line, "/mask", 5)) {
            comlinMaskModeEnable(state);
        } else if (!strncmp(line, "/unmask", 7)) {
            comlinMaskModeDisable(state);
        } else if (line[0] == '/') {
            printString("Unreconized command: ");
            printString(line);
            printString("\n");
        }
        free(line);
    }

    comlinFreeState(state);
    return 0;
}
