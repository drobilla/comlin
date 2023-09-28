// Copyright 2023 David Robillard <d@drobilla.net>
// Copyright 2010-2023 Salvatore Sanfilippo <antirez@gmail.com>
// Copyright 2010-2013 Pieter Noordhuis <pcnoordhuis@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause

/* Makes a number of assumptions that happen to be true on virtually any
 * remotely modern POSIX system.
 *
 * References:
 * - http://invisible-island.net/xterm/ctlseqs/ctlseqs.html
 * - http://www.3waylabs.com/nw/WWW/products/wizcon/vt220.html
 *
 * TODO:
 * - Filter bogus Ctrl+<char> combinations.
 * - Add Win32 support.
 */

#include "comlin/comlin.h"

#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMLIN_DEFAULT_HISTORY_MAX_LEN 100
#define COMLIN_MAX_LINE 4096

struct ComlinStateImpl {
    // Completion
    ComlinCompletionCallback* completionCallback; ///< Get completions

    // Terminal session state
    int ifd;       ///< Terminal stdin file descriptor
    int ofd;       ///< Terminal stdout file descriptor
    size_t cols;   ///< Number of columns in terminal
    bool maskmode; ///< Show asterisks instead of input (for passwords)
    bool rawmode;  ///< Terminal is currently in raw mode
    bool mlmode;   ///< Multi-line mode (default is single line)

    // History
    size_t history_max_len; ///< Maximum number of history entries to keep
    size_t history_len;     ///< Number of history entries
    char** history;         ///< History entries

    // Line editing state
    struct termios cooked; ///< Terminal settings before raw mode
    char* buf;             ///< Editing line buffer
    size_t buflen;         ///< Editing line buffer size
    const char* prompt;    ///< Prompt to display
    size_t plen;           ///< Prompt length
    size_t pos;            ///< Current cursor position
    size_t len;            ///< Current edited line length
    size_t history_index;  ///< The history index we're currently editing
    bool in_completion;    ///< Currently doing a completion
    size_t completion_idx; ///< Index of next completion to propose

    // Multi-line refresh state
    size_t oldpos;  ///< Previous refresh cursor position
    size_t oldrows; ///< Rows used by last refreshed line (multi-line)
};

static const char* const unsupported_term[] = {"dumb", "cons25", "emacs", NULL};

static char*
comlinNoTTY(void);

static void
refreshLineWithCompletion(ComlinState* ls,
                          ComlinCompletions* lc,
                          unsigned flags);

static void
refreshLineWithFlags(ComlinState* l, unsigned flags);

enum KEY_ACTION {
    KEY_NULL = 0,   // NULL
    CTRL_A = 1,     // Ctrl+a
    CTRL_B = 2,     // Ctrl-b
    CTRL_C = 3,     // Ctrl-c
    CTRL_D = 4,     // Ctrl-d
    CTRL_E = 5,     // Ctrl-e
    CTRL_F = 6,     // Ctrl-f
    CTRL_H = 8,     // Ctrl-h
    TAB = 9,        // Tab
    CTRL_K = 11,    // Ctrl+k
    CTRL_L = 12,    // Ctrl+l
    ENTER = 13,     // Enter
    CTRL_N = 14,    // Ctrl-n
    CTRL_P = 16,    // Ctrl-p
    CTRL_T = 20,    // Ctrl-t
    CTRL_U = 21,    // Ctrl+u
    CTRL_W = 23,    // Ctrl+w
    ESC = 27,       // Escape
    BACKSPACE = 127 // Backspace
};

#define REFRESH_CLEAN (1U << 0U) // Clean the old prompt from the screen
#define REFRESH_WRITE (1U << 1U) // Rewrite the prompt on the screen.
#define REFRESH_ALL (REFRESH_CLEAN | REFRESH_WRITE) // Do both.

static void
refreshLine(ComlinState* l);

/* ======================= Low level terminal handling ====================== */

void
comlinMaskModeEnable(ComlinState* const comlin)
{
    comlin->maskmode = true;
}

void
comlinMaskModeDisable(ComlinState* const comlin)
{
    comlin->maskmode = false;
}

void
comlinSetMultiLine(ComlinState* const comlin, const bool ml)
{
    comlin->mlmode = ml;
}

/* Return true if the terminal name is in the list of terminals we know are
 * not able to understand basic escape sequences. */
static bool
isUnsupportedTerm(void)
{
    char* term = getenv("TERM");

    if (term == NULL) {
        return false;
    }

    for (int j = 0; unsupported_term[j]; ++j) {
        if (!strcasecmp(term, unsupported_term[j])) {
            return true;
        }
    }

    return false;
}

static int
write_string(const int fd, const char* const buf, const size_t count)
{
    size_t offset = 0U;
    size_t remaining = count;
    while (remaining > 0) {
        const ssize_t r = write(fd, buf + offset, remaining);
        if (r < 0) {
            return -1;
        }

        remaining -= (size_t)r;
        offset += (size_t)r;
    }

    return 0;
}

/// Set terminal to raw input mode and preserve the original settings
static int
enableRawMode(ComlinState* const state)
{
    // Save original terminal attributes
    if (isatty(state->ifd) && tcgetattr(state->ifd, &state->cooked) == -1) {
        return -1;
    }

    struct termios raw = state->cooked;
    raw.c_iflag &= ~(tcflag_t)BRKINT; // No break
    raw.c_iflag &= ~(tcflag_t)ICRNL;  // No CR to NL
    raw.c_iflag &= ~(tcflag_t)INPCK;  // No parity check
    raw.c_iflag &= ~(tcflag_t)ISTRIP; // No strip char
    raw.c_iflag &= ~(tcflag_t)IXON;   // No flow control
    raw.c_oflag &= ~(tcflag_t)OPOST;  // No post processing
    raw.c_cflag |= (tcflag_t)CS8;     // 8 bit characters
    raw.c_lflag &= ~(tcflag_t)ECHO;   // No echo
    raw.c_lflag &= ~(tcflag_t)ICANON; // No canonical mode
    raw.c_lflag &= ~(tcflag_t)IEXTEN; // No extended functions
    raw.c_lflag &= ~(tcflag_t)ISIG;   // No signal chars (^Z, ^C)
    raw.c_cc[VMIN] = 1;               // Minimum 1 byte read
    raw.c_cc[VTIME] = 0;              // No read timeout

    // Set raw terminal mode after flushing
    const int rc = tcsetattr(state->ifd, TCSAFLUSH, &raw);
    state->rawmode = !rc;
    return rc;
}

static void
disableRawMode(ComlinState* const state)
{
    // Don't even check the return value as it's too late
    if (state->rawmode &&
        tcsetattr(state->ifd, TCSAFLUSH, &state->cooked) != -1) {
        state->rawmode = false;
    }
}

/* Use the ESC [6n escape sequence to query the horizontal cursor position
 * and return it. On error -1 is returned, on success the position of the
 * cursor. */
static int
getCursorPosition(int ifd, int ofd)
{
    char buf[32];
    int cols = 0;
    int rows = 0;
    unsigned int i = 0;

    // Report cursor location
    if (write_string(ofd, "\x1B[6n", 4)) {
        return -1;
    }

    // Read the response: ESC [ rows ; cols R
    while (i + 1U < sizeof(buf)) {
        if (read(ifd, buf + i, 1) != 1) {
            break;
        }
        if (buf[i] == 'R') {
            break;
        }
        ++i;
    }
    buf[i] = '\0';

    // Parse it
    if (buf[0] != ESC || buf[1] != '[') {
        return -1;
    }

    if (sscanf(buf + 2, "%d;%d", &rows, &cols) != 2) {
        return -1;
    }

    return cols;
}

/* Try to get the number of columns in the current terminal, or assume 80
 * if it fails. */
static int
getColumns(int ifd, int ofd)
{
    struct winsize ws;

    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // ioctl() failed. Try to query the terminal itself

        // Get the initial position so we can restore it later
        int start = getCursorPosition(ifd, ofd);
        if (start == -1) {
            goto failed;
        }

        // Go to right margin and get position
        if (write_string(ofd, "\x1B[999C", 6)) {
            goto failed;
        }
        int cols = getCursorPosition(ifd, ofd);
        if (cols == -1) {
            goto failed;
        }

        // Restore position
        if (cols > start) {
            char seq[32];
            snprintf(seq, 32, "\x1B[%dD", cols - start);
            if (write_string(ofd, seq, strlen(seq))) {
                // Failed to restore position, oh well
            }
        }
        return cols;
    }

    return ws.ws_col;

failed:
    return 80;
}

void
comlinClearScreen(ComlinState* const state)
{
    if (write_string(state->ofd, "\x1B[H\x1B[2J", 7)) {
        // Failed to clear screen, oh well
    }
}

/* Beep, used for completion when there is nothing to complete or when all
 * the choices were already shown. */
static void
comlinBeep(ComlinState* const state)
{
    if (write(state->ofd, "\x07", 1) != 1) {
        // Failed to write ASCII BEL, oh well
    }
}

/* ============================== Completion ================================ */

// Free a list of completion option populated by comlinAddCompletion()
static void
freeCompletions(ComlinCompletions* lc)
{
    for (size_t i = 0U; i < lc->len; ++i) {
        free(lc->cvec[i]);
    }
    if (lc->cvec != NULL) {
        free(lc->cvec);
    }
}

/* Called by completeLine() and comlinShow() to render the current
 * edited line with the proposed completion. If the current completion table
 * is already available, it is passed as second argument, otherwise the
 * function will use the callback to obtain it.
 *
 * Flags are the same as refreshLine*(), that is REFRESH_* macros. */
static void
refreshLineWithCompletion(ComlinState* const ls,
                          ComlinCompletions* lc,
                          const unsigned flags)
{
    // Obtain the table of completions if the caller didn't provide one
    ComlinCompletions ctable = {0, NULL};
    if (lc == NULL) {
        ls->completionCallback(ls->buf, &ctable);
        lc = &ctable;
    }

    // Show the edited line with completion if possible, or just refresh
    if (ls->completion_idx < lc->len) {
        ComlinState saved = *ls;
        ls->len = ls->pos = strlen(lc->cvec[ls->completion_idx]);
        ls->buf = lc->cvec[ls->completion_idx];
        refreshLineWithFlags(ls, flags);
        ls->len = saved.len;
        ls->pos = saved.pos;
        ls->buf = saved.buf;
    } else {
        refreshLineWithFlags(ls, flags);
    }

    // Free the completions table if needed
    if (lc != &ctable) {
        freeCompletions(&ctable);
    }
}

/* This is an helper function for comlinEdit*() and is called when the
 * user types the <tab> key in order to complete the string currently in the
 * input.
 *
 * The state of the editing is encapsulated into the pointed comlinState
 * structure as described in the structure definition.
 *
 * If the function returns non-zero, the caller should handle the
 * returned value as a byte read from the standard input, and process
 * it as usually: this basically means that the function may return a byte
 * read from the termianl but not processed. Otherwise, if zero is returned,
 * the input was consumed by the completeLine() function to navigate the
 * possible completions, and the caller should read for the next characters
 * from stdin. */
static char
completeLine(ComlinState* const ls, char keypressed)
{
    ComlinCompletions lc = {0, NULL};
    char c = keypressed;

    ls->completionCallback(ls->buf, &lc);
    if (lc.len == 0) {
        comlinBeep(ls);
        ls->in_completion = false;
    } else {
        switch (c) {
        case TAB: // Tab
            if (!ls->in_completion) {
                ls->in_completion = true;
                ls->completion_idx = 0;
            } else {
                ls->completion_idx = (ls->completion_idx + 1) % (lc.len + 1);
                if (ls->completion_idx == lc.len) {
                    comlinBeep(ls);
                }
            }
            c = 0;
            break;
        case ESC: // Escape
            // Re-show original buffer
            if (ls->completion_idx < lc.len) {
                refreshLine(ls);
            }
            ls->in_completion = false;
            c = 0;
            break;
        default:
            // Update buffer and return
            if (ls->completion_idx < lc.len) {
                const int nwritten = snprintf(
                  ls->buf, ls->buflen, "%s", lc.cvec[ls->completion_idx]);
                assert(nwritten > 0);
                ls->len = ls->pos = (size_t)nwritten;
            }
            ls->in_completion = false;
            break;
        }

        // Show completion or original buffer
        if (ls->in_completion && ls->completion_idx < lc.len) {
            refreshLineWithCompletion(ls, &lc, REFRESH_ALL);
        } else {
            refreshLine(ls);
        }
    }

    freeCompletions(&lc);
    return c; // Return last read character
}

void
comlinSetCompletionCallback(ComlinState* const state,
                            ComlinCompletionCallback* const fn)
{
    state->completionCallback = fn;
}

void
comlinAddCompletion(ComlinCompletions* lc, const char* str)
{
    size_t len = strlen(str);

    char* copy = (char*)malloc(len + 1);
    if (copy == NULL) {
        return;
    }

    memcpy(copy, str, len + 1);
    char** cvec = (char**)realloc(lc->cvec, sizeof(char*) * (lc->len + 1));
    if (cvec == NULL) {
        free(copy);
        return;
    }
    lc->cvec = cvec;
    lc->cvec[lc->len++] = copy;
}

/* =========================== Line editing ================================= */

/* We define a very simple "append buffer" structure, that is an heap
 * allocated string where we can append to. This is useful in order to
 * write all the escape sequences in a buffer and flush them to the standard
 * output in a single call, to avoid flickering effects. */
struct abuf {
    char* data;    ///< Pointer to string buffer
    size_t length; ///< Length of string (not greater than size)
    size_t size;   ///< Size of data
};

static void
abAppend(struct abuf* const ab, const char* const s, const size_t len)
{
    const size_t new_length = ab->length + len;
    const size_t needed_size = new_length + 1U;
    if (needed_size > ab->size) {
        char* const new_data = (char*)realloc(ab->data, needed_size);
        if (!new_data) {
            return;
        }

        ab->data = new_data;
        ab->size = needed_size;
    }

    memcpy(ab->data + ab->length, s, len);
    ab->data[new_length] = '\0';
    ab->length = new_length;
}

static void
abFree(struct abuf* ab)
{
    free(ab->data);
}

/* Single line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal.
 *
 * Flags is REFRESH_* macros. The function can just remove the old
 * prompt, just write it, or both. */
static void
refreshSingleLine(ComlinState* const l, unsigned flags)
{
    char seq[64];
    size_t plen = strlen(l->prompt);
    int fd = l->ofd;
    char* buf = l->buf;
    size_t len = l->len;
    size_t pos = l->pos;

    while ((plen + pos) >= l->cols) {
        ++buf;
        --len;
        --pos;
    }
    while (plen + len > l->cols) {
        --len;
    }

    // Cursor to left edge
    struct abuf ab = {NULL, 0U, 0U};
    snprintf(seq, sizeof(seq), "\r");
    abAppend(&ab, seq, strlen(seq));

    if (flags & REFRESH_WRITE) {
        // Write the prompt and the current buffer content
        abAppend(&ab, l->prompt, strlen(l->prompt));
        if (l->maskmode) {
            while (len--) {
                abAppend(&ab, "*", 1);
            }
        } else {
            abAppend(&ab, buf, len);
        }
    }

    // Erase to right
    snprintf(seq, sizeof(seq), "\x1B[0K");
    abAppend(&ab, seq, strlen(seq));

    if (flags & REFRESH_WRITE) {
        // Move cursor to original position
        snprintf(seq, sizeof(seq), "\r\x1B[%dC", (int)(pos + plen));
        abAppend(&ab, seq, strlen(seq));
    }

    if (write_string(fd, ab.data, ab.length)) {
        // Failed to write to terminal, this is bad and should be reported...
    }

    abFree(&ab);
}

/* Multi line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal.
 *
 * Flags is REFRESH_* macros. The function can just remove the old
 * prompt, just write it, or both. */
static void
refreshMultiLine(ComlinState* const l, unsigned flags)
{
    char seq[64];
    const size_t plen = strlen(l->prompt);
    size_t rows =
      (plen + l->len + l->cols - 1U) / l->cols;           // Rows in current buf
    size_t rpos = (plen + l->oldpos + l->cols) / l->cols; // Cursor relative row
    size_t old_rows = l->oldrows;
    int fd = l->ofd;

    l->oldrows = rows;

    // We'll build the update here, then send it all in a single write
    struct abuf ab = {NULL, 0U, 0U};

    /* First step: clear all the lines used before. To do so start by
     * going to the last row. */
    if (flags & REFRESH_CLEAN) {
        if (old_rows > rpos) {
            snprintf(seq, 64, "\x1B[%zuB", old_rows - rpos);
            abAppend(&ab, seq, strlen(seq));
        }

        // Now for every row clear it, go up
        for (size_t j = 1U; j < old_rows; ++j) {
            snprintf(seq, 64, "\r\x1B[0K\x1B[1A");
            abAppend(&ab, seq, strlen(seq));
        }
    }

    if (flags & REFRESH_ALL) {
        // Clean the top line
        snprintf(seq, 64, "\r\x1B[0K");
        abAppend(&ab, seq, strlen(seq));
    }

    if (flags & REFRESH_WRITE) {
        // Write the prompt and the current buffer content
        abAppend(&ab, l->prompt, strlen(l->prompt));
        if (l->maskmode) {
            for (unsigned i = 0; i < l->len; ++i) {
                abAppend(&ab, "*", 1);
            }
        } else {
            abAppend(&ab, l->buf, l->len);
        }

        /* If we are at the very end of the screen with our prompt, we need to
         * emit a newline and move the prompt to the first column. */
        if (l->pos && l->pos == l->len && (l->pos + plen) % l->cols == 0) {
            abAppend(&ab, "\n", 1);
            snprintf(seq, 64, "\r");
            abAppend(&ab, seq, strlen(seq));
            ++rows;
            if (rows > l->oldrows) {
                l->oldrows = rows;
            }
        }

        // Move cursor to right position
        const size_t rpos2 =
          (plen + l->pos + l->cols) / l->cols; // Current cursor relative row

        // Go up till we reach the expected position
        if (rows > rpos2) {
            snprintf(seq, 64, "\x1B[%zuA", rows - rpos2);
            abAppend(&ab, seq, strlen(seq));
        }

        // Set column
        const size_t col = (plen + l->pos) % l->cols;
        if (col) {
            snprintf(seq, 64, "\r\x1B[%zuC", col);
        } else {
            snprintf(seq, 64, "\r");
        }
        abAppend(&ab, seq, strlen(seq));
    }

    l->oldpos = l->pos;

    if (write_string(fd, ab.data, ab.length)) {
        // Failed to write to terminal, this is bad and should be reported...
    }

    abFree(&ab);
}

/* Calls the two low level functions refreshSingleLine() or
 * refreshMultiLine() according to the selected mode. */
static void
refreshLineWithFlags(ComlinState* const l, unsigned flags)
{
    if (l->mlmode) {
        refreshMultiLine(l, flags);
    } else {
        refreshSingleLine(l, flags);
    }
}

// Utility function to avoid specifying REFRESH_ALL all the times
static void
refreshLine(ComlinState* const l)
{
    refreshLineWithFlags(l, REFRESH_ALL);
}

void
comlinHide(ComlinState* const l)
{
    if (l->mlmode) {
        refreshMultiLine(l, REFRESH_CLEAN);
    } else {
        refreshSingleLine(l, REFRESH_CLEAN);
    }
}

void
comlinShow(ComlinState* const l)
{
    if (l->in_completion) {
        refreshLineWithCompletion(l, NULL, REFRESH_WRITE);
    } else {
        refreshLineWithFlags(l, REFRESH_WRITE);
    }
}

/* Insert the character 'c' at cursor current position.
 *
 * On error writing to the terminal -1 is returned, otherwise 0. */
static int
comlinEditInsert(ComlinState* const l, char c)
{
    if (l->len < l->buflen) {
        if (l->len == l->pos) {
            l->buf[l->pos] = c;
            ++l->pos;
            ++l->len;
            l->buf[l->len] = '\0';
            if (!l->mlmode && l->plen + l->len < l->cols) {
                /* Avoid a full update of the line in the
                 * trivial case. */
                char d = (char)(l->maskmode ? '*' : c);
                if (write(l->ofd, &d, 1) != 1) {
                    return -1;
                }
            } else {
                refreshLine(l);
            }
        } else {
            memmove(l->buf + l->pos + 1, l->buf + l->pos, l->len - l->pos);
            l->buf[l->pos] = c;
            ++l->len;
            ++l->pos;
            l->buf[l->len] = '\0';
            refreshLine(l);
        }
    }
    return 0;
}

// Move cursor on the left
static void
comlinEditMoveLeft(ComlinState* const l)
{
    if (l->pos > 0) {
        --l->pos;
        refreshLine(l);
    }
}

// Move cursor on the right
static void
comlinEditMoveRight(ComlinState* const l)
{
    if (l->pos != l->len) {
        ++l->pos;
        refreshLine(l);
    }
}

// Move cursor to the start of the line
static void
comlinEditMoveHome(ComlinState* const l)
{
    if (l->pos != 0) {
        l->pos = 0;
        refreshLine(l);
    }
}

// Move cursor to the end of the line
static void
comlinEditMoveEnd(ComlinState* const l)
{
    if (l->pos != l->len) {
        l->pos = l->len;
        refreshLine(l);
    }
}

/* Substitute the currently edited line with the next or previous history
 * entry as specified by 'dir'. */
#define COMLIN_HISTORY_NEXT 0
#define COMLIN_HISTORY_PREV 1
static void
comlinEditHistoryNext(ComlinState* const l, int dir)
{
    if (l->history_len > 1U) {
        // Update the current history entry before overwriting it with the next
        free(l->history[l->history_len - 1U - l->history_index]);
        l->history[l->history_len - 1U - l->history_index] = strdup(l->buf);

        // Show the new entry
        if (dir == COMLIN_HISTORY_NEXT) {
            if (l->history_index == 0) {
                return;
            }
            --l->history_index;
        } else {
            ++l->history_index;
        }
        if (l->history_index >= l->history_len) {
            l->history_index = l->history_len - 1U;
            return;
        }
        strncpy(l->buf,
                l->history[l->history_len - 1U - l->history_index],
                l->buflen);
        l->buf[l->buflen - 1U] = '\0';
        l->len = l->pos = strlen(l->buf);
        refreshLine(l);
    }
}

/* Delete the character at the right of the cursor without altering the cursor
 * position. Basically this is what happens with the "Delete" keyboard key. */
static void
comlinEditDelete(ComlinState* const l)
{
    if (l->len > 0 && l->pos < l->len) {
        memmove(l->buf + l->pos, l->buf + l->pos + 1U, l->len - l->pos - 1U);
        --l->len;
        l->buf[l->len] = '\0';
        refreshLine(l);
    }
}

// Backspace implementation
static void
comlinEditBackspace(ComlinState* const l)
{
    if (l->pos > 0 && l->len > 0) {
        memmove(l->buf + l->pos - 1U, l->buf + l->pos, l->len - l->pos);
        --l->pos;
        --l->len;
        l->buf[l->len] = '\0';
        refreshLine(l);
    }
}

/* Delete the previous word, maintaining the cursor at the start of the
 * current word. */
static void
comlinEditDeletePrevWord(ComlinState* const l)
{
    size_t old_pos = l->pos;

    while (l->pos > 0 && l->buf[l->pos - 1U] == ' ') {
        --l->pos;
    }
    while (l->pos > 0 && l->buf[l->pos - 1U] != ' ') {
        --l->pos;
    }
    const size_t diff = old_pos - l->pos;
    memmove(l->buf + l->pos, l->buf + old_pos, l->len + 1U - old_pos);
    l->len -= diff;
    refreshLine(l);
}

ComlinState*
comlinNewState(const int stdin_fd,
               const int stdout_fd,
               char* const buf,
               const size_t buflen,
               const char* prompt)
{
    ComlinState* const l = (ComlinState*)calloc(1, sizeof(ComlinState));
    if (!l) {
        return NULL;
    }

    l->ifd = stdin_fd != -1 ? stdin_fd : STDIN_FILENO;
    l->ofd = stdout_fd != -1 ? stdout_fd : STDOUT_FILENO;
    l->history_max_len = COMLIN_DEFAULT_HISTORY_MAX_LEN;
    l->buf = buf;
    l->buflen = buflen;
    l->prompt = prompt;
    l->plen = strlen(prompt);
    l->cols = (size_t)getColumns(stdin_fd, stdout_fd);

    // Buffer starts empty
    l->buf[0] = '\0';
    --l->buflen; // Make sure there is always space for the nulterm

    return l;
}

int
comlinEditStart(ComlinState* const l)
{
    // Enter raw mode
    if (isatty(l->ifd) && enableRawMode(l) == -1) {
        return -1;
    }

    // Reset line state
    l->rawmode = true;
    l->buf[0] = '\0';
    l->pos = 0U;
    l->oldpos = 0U;
    l->len = 0U;
    l->oldrows = 0U;

    /* The latest history entry is always our current buffer, that
     * initially is just an empty string. */
    comlinHistoryAdd(l, "");

    // Write prompt
    return write_string(l->ofd, l->prompt, l->plen);
}

char* comlinEditMore =
  "If you see this, you are misusing the API: when comlinEditFeed() is "
  "called, if it returns comlinEditMore the user is yet editing the line. "
  "See the README file for more information.";

char*
comlinEditFeed(ComlinState* const l)
{
    /* Not a TTY, pass control to line reading without character
     * count limits. */
    if (!isatty(l->ifd)) {
        return comlinNoTTY();
    }

    char c = '\0';
    char seq[3] = {'\0', '\0', '\0'};

    const ssize_t nread = read(l->ifd, &c, 1);
    if (nread <= 0) {
        return NULL;
    }

    /* Only autocomplete when the callback is set. It returns < 0 when
     * there was an error reading from fd. Otherwise it will return the
     * character that should be handled next. */
    if ((l->in_completion || c == TAB) && l->completionCallback != NULL) {
        c = completeLine(l, c);
        // Return on errors
        if (c < 0) {
            return NULL;
        }
        // Read next character when 0
        if (c == 0) {
            return comlinEditMore;
        }
    }

    switch (c) {
    case ENTER: // Enter
        --l->history_len;
        free(l->history[l->history_len]);
        if (l->mlmode) {
            comlinEditMoveEnd(l);
        }
        return strdup(l->buf);
    case CTRL_C: // Ctrl-c
        errno = EAGAIN;
        return NULL;
    case BACKSPACE: // Backspace
    case CTRL_H:    // Ctrl-h
        comlinEditBackspace(l);
        break;
    case CTRL_D: /* ctrl-d, remove char at right of cursor, or if the
                    line is empty, act as end-of-file. */
        if (l->len > 0) {
            comlinEditDelete(l);
        } else {
            --l->history_len;
            free(l->history[l->history_len]);
            errno = ENOENT;
            return NULL;
        }
        break;
    case CTRL_T: // Ctrl-t, swaps current character with previous
        if (l->pos > 0 && l->pos < l->len) {
            const char aux = l->buf[l->pos - 1U];
            l->buf[l->pos - 1U] = l->buf[l->pos];
            l->buf[l->pos] = aux;
            if (l->pos != l->len - 1U) {
                ++l->pos;
            }
            refreshLine(l);
        }
        break;
    case CTRL_B: // Ctrl-b
        comlinEditMoveLeft(l);
        break;
    case CTRL_F: // Ctrl-f
        comlinEditMoveRight(l);
        break;
    case CTRL_P: // Ctrl-p
        comlinEditHistoryNext(l, COMLIN_HISTORY_PREV);
        break;
    case CTRL_N: // Ctrl-n
        comlinEditHistoryNext(l, COMLIN_HISTORY_NEXT);
        break;
    case ESC: // Escape sequence
        /* Read the next two bytes representing the escape sequence.
         * Use two calls to handle slow terminals returning the two
         * chars at different times. */
        if (read(l->ifd, seq, 1) == -1) {
            break;
        }
        if (read(l->ifd, seq + 1, 1) == -1) {
            break;
        }

        // ESC [ sequences
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                // Extended escape, read additional byte
                if (read(l->ifd, seq + 2, 1) == -1) {
                    break;
                }
                if (seq[2] == '~') {
                    switch (seq[1]) {
                    case '3': // Delete key
                        comlinEditDelete(l);
                        break;
                    }
                }
            } else {
                switch (seq[1]) {
                case 'A': // Up
                    comlinEditHistoryNext(l, COMLIN_HISTORY_PREV);
                    break;
                case 'B': // Down
                    comlinEditHistoryNext(l, COMLIN_HISTORY_NEXT);
                    break;
                case 'C': // Right
                    comlinEditMoveRight(l);
                    break;
                case 'D': // Left
                    comlinEditMoveLeft(l);
                    break;
                case 'H': // Home
                    comlinEditMoveHome(l);
                    break;
                case 'F': // End
                    comlinEditMoveEnd(l);
                    break;
                }
            }
        }

        // ESC O sequences
        else if (seq[0] == 'O') {
            switch (seq[1]) {
            case 'H': // Home
                comlinEditMoveHome(l);
                break;
            case 'F': // End
                comlinEditMoveEnd(l);
                break;
            }
        }
        break;
    default:
        if (comlinEditInsert(l, c)) {
            return NULL;
        }
        break;
    case CTRL_U: // Ctrl+u, delete the whole line
        l->buf[0] = '\0';
        l->pos = l->len = 0;
        refreshLine(l);
        break;
    case CTRL_K: // Ctrl+k, delete from current to end of line
        l->buf[l->pos] = '\0';
        l->len = l->pos;
        refreshLine(l);
        break;
    case CTRL_A: // Ctrl+a, go to the start of the line
        comlinEditMoveHome(l);
        break;
    case CTRL_E: // Ctrl+e, go to the end of the line
        comlinEditMoveEnd(l);
        break;
    case CTRL_L: // Ctrl+l, clear screen
        comlinClearScreen(l);
        refreshLine(l);
        break;
    case CTRL_W: // Ctrl+w, delete previous word
        comlinEditDeletePrevWord(l);
        break;
    }
    return comlinEditMore;
}

void
comlinEditStop(ComlinState* const l)
{
    if (l->rawmode) {
        disableRawMode(l);
        if (write(l->ofd, "\n", 1) == -1) {
            // Failed to write trailing newline, oh well
        }
    }
}

const char*
comlinText(ComlinState* const l)
{
    return l->buf;
}

/* This just implements a blocking loop for the multiplexed API.
 * In many applications that are not event-driven, we can just call
 * the blocking comlin API, wait for the user to complete the editing
 * and return the buffer. */
static char*
comlinBlockingEdit(ComlinState* const state)
{
    // Editing without a buffer is invalid
    if (!state->buflen) {
        errno = EINVAL;
        return NULL;
    }

    comlinEditStart(state);
    char* res = NULL;
    while ((res = comlinEditFeed(state)) == comlinEditMore) {
    }
    comlinEditStop(state);
    return res;
}

/* This function is called when comlin() is called with the standard
 * input file descriptor not attached to a TTY. So for example when the
 * program using comlin is called in pipe or with a file redirected
 * to its standard input. In this case, we want to be able to return the
 * line regardless of its length (by default we are limited to 4k). */
static char*
comlinNoTTY(void)
{
    char* line = NULL;
    size_t len = 0;
    size_t maxlen = 0;

    while (1) {
        if (len == maxlen) {
            if (maxlen == 0) {
                maxlen = 16;
            }
            maxlen *= 2;
            char* oldval = line;
            line = (char*)realloc(line, maxlen);
            if (line == NULL) {
                if (oldval) {
                    free(oldval);
                }
                return NULL;
            }
        }
        int c = fgetc(stdin);
        if (c == EOF || c == '\n') {
            if (c == EOF && len == 0) {
                free(line);
                return NULL;
            }
            line[len] = '\0';
            return line;
        }

        line[len] = (char)c;
        ++len;
    }
}

char*
comlinReadLine(ComlinState* const state, const char* const prompt)
{
    char buf[COMLIN_MAX_LINE];

    if (!isatty(STDIN_FILENO)) {
        /* Not a tty: read from file / pipe. In this mode we don't want any
         * limit to the line size, so we call a function to handle that. */
        return comlinNoTTY();
    }

    if (isUnsupportedTerm()) {
        const size_t prompt_len = strlen(prompt);
        if (write_string(state->ofd, prompt, prompt_len)) {
            return NULL;
        }
        if (fgets(buf, COMLIN_MAX_LINE, stdin) == NULL) {
            return NULL;
        }
        size_t len = strlen(buf);
        while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
            --len;
            buf[len] = '\0';
        }
        return strdup(buf);
    }

    char* retval = comlinBlockingEdit(state);
    return retval;
}

void
comlinFreeCommand(void* ptr)
{
    if (ptr == comlinEditMore) {
        return; // Protect from API misuse.
    }
    free(ptr);
}

/* ================================ History ================================= */

/* Uses a fixed array of char pointers that are shifted (memmoved)
 * when the history max length is reached in order to remove the older
 * entry and make room for the new one, so it is not exactly suitable for huge
 * histories, but will work well for a few hundred of entries.
 *
 * Using a circular buffer is smarter, but a bit more complex to handle. */
int
comlinHistoryAdd(ComlinState* const state, const char* line)
{
    if (state->history_max_len == 0) {
        return 0;
    }

    // Initialization on first call
    if (state->history == NULL) {
        state->history = (char**)malloc(sizeof(char*) * state->history_max_len);
        if (state->history == NULL) {
            return 0;
        }
        memset(state->history, 0, (sizeof(char*) * state->history_max_len));
    }

    // Don't add duplicated lines
    if (state->history_len &&
        !strcmp(state->history[state->history_len - 1U], line)) {
        return 0;
    }

    /* Add an heap allocated copy of the line in the history.
     * If we reached the max length, remove the older line. */
    char* linecopy = strdup(line);
    if (!linecopy) {
        return 0;
    }
    if (state->history_len == state->history_max_len) {
        free(state->history[0]);
        memmove(state->history,
                state->history + 1,
                sizeof(char*) * (state->history_max_len - 1U));
        --state->history_len;
    }
    state->history[state->history_len] = linecopy;
    ++state->history_len;
    return 1;
}

int
comlinHistorySetMaxLen(ComlinState* const state, const size_t len)
{
    if (len < 1) {
        return 0;
    }
    if (state->history) {
        size_t tocopy = state->history_len;

        char** new_history = (char**)malloc(sizeof(char*) * len);
        if (new_history == NULL) {
            return 0;
        }

        // If we can't copy everything, free the elements we'll not use
        if (len < tocopy) {
            for (size_t j = 0U; j < tocopy - len; ++j) {
                free(state->history[j]);
            }
            tocopy = len;
        }
        memset(new_history, 0, sizeof(char*) * len);
        memcpy(new_history,
               state->history + (state->history_len - tocopy),
               sizeof(char*) * tocopy);
        free(state->history);
        state->history = new_history;
    }
    state->history_max_len = len;
    if (state->history_len > state->history_max_len) {
        state->history_len = state->history_max_len;
    }
    return 1;
}

int
comlinHistorySave(const ComlinState* const state, const char* filename)
{
    mode_t old_umask = umask(S_IXUSR | S_IRWXG | S_IRWXO);
    FILE* fp = fopen(filename, "w");
    umask(old_umask);
    if (fp == NULL) {
        return -1;
    }
    chmod(filename, S_IRUSR | S_IWUSR);
    for (size_t j = 0U; j < state->history_len; ++j) {
        fprintf(fp, "%s\n", state->history[j]);
    }
    fclose(fp);
    return 0;
}

int
comlinHistoryLoad(ComlinState* const state, const char* filename)
{
    FILE* fp = fopen(filename, "r");
    char buf[COMLIN_MAX_LINE];

    if (fp == NULL) {
        return -1;
    }

    while (fgets(buf, COMLIN_MAX_LINE, fp) != NULL) {
        char* p = strchr(buf, '\r');
        if (!p) {
            p = strchr(buf, '\n');
        }
        if (p) {
            *p = '\0';
        }
        comlinHistoryAdd(state, buf);
    }
    fclose(fp);
    return 0;
}

void
comlinFreeState(ComlinState* const state)
{
    // Free history
    for (size_t j = 0U; j < state->history_len; ++j) {
        free(state->history[j]);
    }
    free(state->history);

    // Disable raw mode if it was enabled by comlinNew
    if (state->rawmode) {
        disableRawMode(state);
    }

    free(state);
}
