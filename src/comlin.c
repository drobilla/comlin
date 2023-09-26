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

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMLIN_DEFAULT_HISTORY_MAX_LEN 100
#define COMLIN_MAX_LINE 4096

static const char *unsupported_term[] = {"dumb", "cons25", "emacs", NULL};
static comlinCompletionCallback *completionCallback = NULL;
static comlinHintsCallback *hintsCallback = NULL;
static comlinFreeHintsCallback *freeHintsCallback = NULL;

static char *
comlinNoTTY(void);

static void
refreshLineWithCompletion(struct comlinState *ls,
                          comlinCompletions *lc,
                          int flags);

static void
refreshLineWithFlags(struct comlinState *l, unsigned flags);

static struct termios orig_termios; /* In order to restore at exit.*/
static int maskmode = 0; // Show "***" instead of input (for passwords)
static int rawmode = 0;  // For atexit() function to check if restore is needed
static int mlmode = 0;   // Multi line mode (default is single line)
static int atexit_registered = 0; // Register atexit just once
static int history_max_len = COMLIN_DEFAULT_HISTORY_MAX_LEN;
static int history_len = 0;
static char **history = NULL;

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

static void
comlinAtExit(void);

#define REFRESH_CLEAN (1U << 0U) // Clean the old prompt from the screen
#define REFRESH_WRITE (1U << 1U) // Rewrite the prompt on the screen.
#define REFRESH_ALL (REFRESH_CLEAN | REFRESH_WRITE) // Do both.

static void
refreshLine(struct comlinState *l);

/* ======================= Low level terminal handling ====================== */

void
comlinMaskModeEnable(void)
{
    maskmode = 1;
}

void
comlinMaskModeDisable(void)
{
    maskmode = 0;
}

void
comlinSetMultiLine(int ml)
{
    mlmode = ml;
}

/* Return true if the terminal name is in the list of terminals we know are
 * not able to understand basic escape sequences. */
static int
isUnsupportedTerm(void)
{
    char *term = getenv("TERM");

    if (term == NULL) {
        return 0;
    }

    for (int j = 0; unsupported_term[j]; ++j) {
        if (!strcasecmp(term, unsupported_term[j])) {
            return 1;
        }
    }

    return 0;
}

// Raw mode: 1960 magic shit
static int
enableRawMode(int fd)
{
    struct termios raw;

    if (!isatty(STDIN_FILENO)) {
        goto fatal;
    }

    if (!atexit_registered) {
        atexit(comlinAtExit);
        atexit_registered = 1;
    }
    if (tcgetattr(fd, &orig_termios) == -1) {
        goto fatal;
    }

    raw = orig_termios;
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
    if (tcsetattr(fd, TCSAFLUSH, &raw) < 0) {
        goto fatal;
    }

    rawmode = 1;
    return 0;

fatal:
    errno = ENOTTY;
    return -1;
}

static void
disableRawMode(int fd)
{
    // Don't even check the return value as it's too late
    if (rawmode && tcsetattr(fd, TCSAFLUSH, &orig_termios) != -1) {
        rawmode = 0;
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
    if (write(ofd, "\x1b[6n", 4) != 4) {
        return -1;
    }

    // Read the response: ESC [ rows ; cols R
    while (i < sizeof(buf) - 1) {
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
        if (write(ofd, "\x1b[999C", 6) != 6) {
            goto failed;
        }
        int cols = getCursorPosition(ifd, ofd);
        if (cols == -1) {
            goto failed;
        }

        // Restore position
        if (cols > start) {
            char seq[32];
            snprintf(seq, 32, "\x1b[%dD", cols - start);
            if (write(ofd, seq, strlen(seq)) == -1) {
                // Can't recover..
            }
        }
        return cols;
    }

    return ws.ws_col;

failed:
    return 80;
}

void
comlinClearScreen(void)
{
    if (write(STDOUT_FILENO, "\x1b[H\x1b[2J", 7) <= 0) {
        // Nothing to do, just to avoid warning
    }
}

/* Beep, used for completion when there is nothing to complete or when all
 * the choices were already shown. */
static void
comlinBeep(void)
{
    fprintf(stderr, "\x7");
    fflush(stderr);
}

/* ============================== Completion ================================ */

// Free a list of completion option populated by comlinAddCompletion()
static void
freeCompletions(comlinCompletions *lc)
{
    for (size_t i = 0; i < lc->len; ++i) {
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
refreshLineWithCompletion(struct comlinState *ls,
                          comlinCompletions *lc,
                          int flags)
{
    // Obtain the table of completions if the caller didn't provide one
    comlinCompletions ctable = {0, NULL};
    if (lc == NULL) {
        completionCallback(ls->buf, &ctable);
        lc = &ctable;
    }

    // Show the edited line with completion if possible, or just refresh
    if (ls->completion_idx < lc->len) {
        struct comlinState saved = *ls;
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
completeLine(struct comlinState *ls, char keypressed)
{
    comlinCompletions lc = {0, NULL};
    char c = keypressed;

    completionCallback(ls->buf, &lc);
    if (lc.len == 0) {
        comlinBeep();
        ls->in_completion = 0;
    } else {
        switch (c) {
        case 9: // Tab
            if (ls->in_completion == 0) {
                ls->in_completion = 1;
                ls->completion_idx = 0;
            } else {
                ls->completion_idx = (ls->completion_idx + 1) % (lc.len + 1);
                if (ls->completion_idx == lc.len) {
                    comlinBeep();
                }
            }
            c = 0;
            break;
        case 27: // Escape
            // Re-show original buffer
            if (ls->completion_idx < lc.len) {
                refreshLine(ls);
            }
            ls->in_completion = 0;
            c = 0;
            break;
        default:
            // Update buffer and return
            if (ls->completion_idx < lc.len) {
                const int nwritten = snprintf(
                  ls->buf, ls->buflen, "%s", lc.cvec[ls->completion_idx]);
                ls->len = ls->pos = nwritten;
            }
            ls->in_completion = 0;
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
comlinSetCompletionCallback(comlinCompletionCallback *fn)
{
    completionCallback = fn;
}

void
comlinSetHintsCallback(comlinHintsCallback *fn)
{
    hintsCallback = fn;
}

void
comlinSetFreeHintsCallback(comlinFreeHintsCallback *fn)
{
    freeHintsCallback = fn;
}

void
comlinAddCompletion(comlinCompletions *lc, const char *str)
{
    size_t len = strlen(str);

    char *copy = (char *)malloc(len + 1);
    if (copy == NULL) {
        return;
    }

    memcpy(copy, str, len + 1);
    char **cvec = (char **)realloc(lc->cvec, sizeof(char *) * (lc->len + 1));
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
    char *b;
    int len;
};

static void
abInit(struct abuf *ab)
{
    ab->b = NULL;
    ab->len = 0;
}

static void
abAppend(struct abuf *ab, const char *s, int len)
{
    char *buf = (char *)realloc(ab->b, ab->len + len);

    if (buf == NULL) {
        return;
    }

    memcpy(buf + ab->len, s, len);
    ab->b = buf;
    ab->len += len;
}

static void
abFree(struct abuf *ab)
{
    free(ab->b);
}

/* Helper of refreshSingleLine() and refreshMultiLine() to show hints
 * to the right of the prompt. */
static void
refreshShowHints(struct abuf *ab, struct comlinState *l, int plen)
{
    char seq[64];
    if (hintsCallback && plen + l->len < l->cols) {
        int color = -1;
        int bold = 0;
        char *hint = hintsCallback(l->buf, &color, &bold);
        if (hint) {
            int hintlen = strlen(hint);
            int hintmaxlen = l->cols - (plen + l->len);
            if (hintlen > hintmaxlen) {
                hintlen = hintmaxlen;
            }
            if (bold == 1 && color == -1) {
                color = 37;
            }
            if (color != -1 || bold != 0) {
                snprintf(seq, 64, "\033[%d;%d;49m", bold, color);
            } else {
                seq[0] = '\0';
            }
            abAppend(ab, seq, strlen(seq));
            abAppend(ab, hint, hintlen);
            if (color != -1 || bold != 0) {
                abAppend(ab, "\033[0m", 4);
            }

            // Call the function to free the hint returned
            if (freeHintsCallback) {
                freeHintsCallback(hint);
            }
        }
    }
}

/* Single line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal.
 *
 * Flags is REFRESH_* macros. The function can just remove the old
 * prompt, just write it, or both. */
static void
refreshSingleLine(struct comlinState *l, unsigned flags)
{
    char seq[64];
    size_t plen = strlen(l->prompt);
    int fd = l->ofd;
    char *buf = l->buf;
    size_t len = l->len;
    size_t pos = l->pos;
    struct abuf ab;

    while ((plen + pos) >= l->cols) {
        ++buf;
        --len;
        --pos;
    }
    while (plen + len > l->cols) {
        --len;
    }

    abInit(&ab);
    // Cursor to left edge
    snprintf(seq, sizeof(seq), "\r");
    abAppend(&ab, seq, strlen(seq));

    if (flags & REFRESH_WRITE) {
        // Write the prompt and the current buffer content
        abAppend(&ab, l->prompt, strlen(l->prompt));
        if (maskmode == 1) {
            while (len--) {
                abAppend(&ab, "*", 1);
            }
        } else {
            abAppend(&ab, buf, len);
        }
        // Show hints if any
        refreshShowHints(&ab, l, plen);
    }

    // Erase to right
    snprintf(seq, sizeof(seq), "\x1b[0K");
    abAppend(&ab, seq, strlen(seq));

    if (flags & REFRESH_WRITE) {
        // Move cursor to original position
        snprintf(seq, sizeof(seq), "\r\x1b[%dC", (int)(pos + plen));
        abAppend(&ab, seq, strlen(seq));
    }

    if (write(fd, ab.b, ab.len) == -1) {
    } // Can't recover from write error
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
refreshMultiLine(struct comlinState *l, unsigned flags)
{
    char seq[64];
    int plen = strlen(l->prompt);
    int rows = (plen + l->len + l->cols - 1) / l->cols; // Rows in current buf
    int rpos = (plen + l->oldpos + l->cols) / l->cols;  // Cursor relative row
    int old_rows = l->oldrows;
    int fd = l->ofd;
    struct abuf ab;

    l->oldrows = rows;

    /* First step: clear all the lines used before. To do so start by
     * going to the last row. */
    abInit(&ab);

    if (flags & REFRESH_CLEAN) {
        if (old_rows - rpos > 0) {
            snprintf(seq, 64, "\x1b[%dB", old_rows - rpos);
            abAppend(&ab, seq, strlen(seq));
        }

        // Now for every row clear it, go up
        for (int j = 0; j < old_rows - 1; ++j) {
            snprintf(seq, 64, "\r\x1b[0K\x1b[1A");
            abAppend(&ab, seq, strlen(seq));
        }
    }

    if (flags & REFRESH_ALL) {
        // Clean the top line
        snprintf(seq, 64, "\r\x1b[0K");
        abAppend(&ab, seq, strlen(seq));
    }

    if (flags & REFRESH_WRITE) {
        // Write the prompt and the current buffer content
        abAppend(&ab, l->prompt, strlen(l->prompt));
        if (maskmode == 1) {
            for (unsigned i = 0; i < l->len; ++i) {
                abAppend(&ab, "*", 1);
            }
        } else {
            abAppend(&ab, l->buf, l->len);
        }

        // Show hints if any
        refreshShowHints(&ab, l, plen);

        /* If we are at the very end of the screen with our prompt, we need to
         * emit a newline and move the prompt to the first column. */
        if (l->pos && l->pos == l->len && (l->pos + plen) % l->cols == 0) {
            abAppend(&ab, "\n", 1);
            snprintf(seq, 64, "\r");
            abAppend(&ab, seq, strlen(seq));
            ++rows;
            if (rows > (int)l->oldrows) {
                l->oldrows = rows;
            }
        }

        // Move cursor to right position
        const int rpos2 =
          (plen + l->pos + l->cols) / l->cols; // Current cursor relative row

        // Go up till we reach the expected position
        if (rows - rpos2 > 0) {
            snprintf(seq, 64, "\x1b[%dA", rows - rpos2);
            abAppend(&ab, seq, strlen(seq));
        }

        // Set column
        const int col = (plen + (int)l->pos) % (int)l->cols;
        if (col) {
            snprintf(seq, 64, "\r\x1b[%dC", col);
        } else {
            snprintf(seq, 64, "\r");
        }
        abAppend(&ab, seq, strlen(seq));
    }

    l->oldpos = l->pos;

    if (write(fd, ab.b, ab.len) == -1) {
    } // Can't recover from write error
    abFree(&ab);
}

/* Calls the two low level functions refreshSingleLine() or
 * refreshMultiLine() according to the selected mode. */
static void
refreshLineWithFlags(struct comlinState *l, unsigned flags)
{
    if (mlmode) {
        refreshMultiLine(l, flags);
    } else {
        refreshSingleLine(l, flags);
    }
}

// Utility function to avoid specifying REFRESH_ALL all the times
static void
refreshLine(struct comlinState *l)
{
    refreshLineWithFlags(l, REFRESH_ALL);
}

void
comlinHide(struct comlinState *l)
{
    if (mlmode) {
        refreshMultiLine(l, REFRESH_CLEAN);
    } else {
        refreshSingleLine(l, REFRESH_CLEAN);
    }
}

void
comlinShow(struct comlinState *l)
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
comlinEditInsert(struct comlinState *l, char c)
{
    if (l->len < l->buflen) {
        if (l->len == l->pos) {
            l->buf[l->pos] = c;
            ++l->pos;
            ++l->len;
            l->buf[l->len] = '\0';
            if ((!mlmode && l->plen + l->len < l->cols && !hintsCallback)) {
                /* Avoid a full update of the line in the
                 * trivial case. */
                char d = (maskmode == 1) ? '*' : c;
                if (write(l->ofd, &d, 1) == -1) {
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
comlinEditMoveLeft(struct comlinState *l)
{
    if (l->pos > 0) {
        --l->pos;
        refreshLine(l);
    }
}

// Move cursor on the right
static void
comlinEditMoveRight(struct comlinState *l)
{
    if (l->pos != l->len) {
        ++l->pos;
        refreshLine(l);
    }
}

// Move cursor to the start of the line
static void
comlinEditMoveHome(struct comlinState *l)
{
    if (l->pos != 0) {
        l->pos = 0;
        refreshLine(l);
    }
}

// Move cursor to the end of the line
static void
comlinEditMoveEnd(struct comlinState *l)
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
comlinEditHistoryNext(struct comlinState *l, int dir)
{
    if (history_len > 1) {
        // Update the current history entry before overwriting it with the next
        free(history[history_len - 1 - l->history_index]);
        history[history_len - 1 - l->history_index] = strdup(l->buf);

        // Show the new entry
        l->history_index += (dir == COMLIN_HISTORY_PREV) ? 1 : -1;
        if (l->history_index < 0) {
            l->history_index = 0;
            return;
        }
        if (l->history_index >= history_len) {
            l->history_index = history_len - 1;
            return;
        }
        strncpy(l->buf, history[history_len - 1 - l->history_index], l->buflen);
        l->buf[l->buflen - 1] = '\0';
        l->len = l->pos = strlen(l->buf);
        refreshLine(l);
    }
}

/* Delete the character at the right of the cursor without altering the cursor
 * position. Basically this is what happens with the "Delete" keyboard key. */
static void
comlinEditDelete(struct comlinState *l)
{
    if (l->len > 0 && l->pos < l->len) {
        memmove(l->buf + l->pos, l->buf + l->pos + 1, l->len - l->pos - 1);
        --l->len;
        l->buf[l->len] = '\0';
        refreshLine(l);
    }
}

// Backspace implementation
static void
comlinEditBackspace(struct comlinState *l)
{
    if (l->pos > 0 && l->len > 0) {
        memmove(l->buf + l->pos - 1, l->buf + l->pos, l->len - l->pos);
        --l->pos;
        --l->len;
        l->buf[l->len] = '\0';
        refreshLine(l);
    }
}

/* Delete the previous word, maintaining the cursor at the start of the
 * current word. */
static void
comlinEditDeletePrevWord(struct comlinState *l)
{
    size_t old_pos = l->pos;

    while (l->pos > 0 && l->buf[l->pos - 1] == ' ') {
        --l->pos;
    }
    while (l->pos > 0 && l->buf[l->pos - 1] != ' ') {
        --l->pos;
    }
    const size_t diff = old_pos - l->pos;
    memmove(l->buf + l->pos, l->buf + old_pos, l->len - old_pos + 1);
    l->len -= diff;
    refreshLine(l);
}

int
comlinEditStart(struct comlinState *l,
                int stdin_fd,
                int stdout_fd,
                char *buf,
                size_t buflen,
                const char *prompt)
{
    /* Populate the state that we pass to functions implementing
     * specific editing functionalities. */
    l->in_completion = 0;
    l->ifd = stdin_fd != -1 ? stdin_fd : STDIN_FILENO;
    l->ofd = stdout_fd != -1 ? stdout_fd : STDOUT_FILENO;
    l->buf = buf;
    l->buflen = buflen;
    l->prompt = prompt;
    l->plen = strlen(prompt);
    l->oldpos = l->pos = 0;
    l->len = 0;
    l->cols = getColumns(stdin_fd, stdout_fd);
    l->oldrows = 0;
    l->history_index = 0;

    // Buffer starts empty
    l->buf[0] = '\0';
    --l->buflen; // Make sure there is always space for the nulterm

    /* If stdin is not a tty, stop here with the initialization. We
     * will actually just read a line from standard input in blocking
     * mode later, in comlinEditFeed(). */
    if (!isatty(l->ifd)) {
        return 0;
    }

    // Enter raw mode
    if (enableRawMode(l->ifd) == -1) {
        return -1;
    }

    /* The latest history entry is always our current buffer, that
     * initially is just an empty string. */
    comlinHistoryAdd("");

    if (write(l->ofd, prompt, l->plen) == -1) {
        return -1;
    }
    return 0;
}

char *comlinEditMore =
  "If you see this, you are misusing the API: when comlinEditFeed() is "
  "called, if it returns comlinEditMore the user is yet editing the line. "
  "See the README file for more information.";

char *
comlinEditFeed(struct comlinState *l)
{
    /* Not a TTY, pass control to line reading without character
     * count limits. */
    if (!isatty(l->ifd)) {
        return comlinNoTTY();
    }

    char c = '\0';
    char seq[3] = {'\0', '\0', '\0'};

    const int nread = read(l->ifd, &c, 1);
    if (nread <= 0) {
        return NULL;
    }

    /* Only autocomplete when the callback is set. It returns < 0 when
     * there was an error reading from fd. Otherwise it will return the
     * character that should be handled next. */
    if ((l->in_completion || c == 9) && completionCallback != NULL) {
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
        --history_len;
        free(history[history_len]);
        if (mlmode) {
            comlinEditMoveEnd(l);
        }
        if (hintsCallback) {
            /* Force a refresh without hints to leave the previous
             * line as the user typed it after a newline. */
            comlinHintsCallback *hc = hintsCallback;
            hintsCallback = NULL;
            refreshLine(l);
            hintsCallback = hc;
        }
        return strdup(l->buf);
    case CTRL_C: // Ctrl-c
        errno = EAGAIN;
        return NULL;
    case BACKSPACE: // Backspace
    case 8:         // Ctrl-h
        comlinEditBackspace(l);
        break;
    case CTRL_D: /* ctrl-d, remove char at right of cursor, or if the
                    line is empty, act as end-of-file. */
        if (l->len > 0) {
            comlinEditDelete(l);
        } else {
            --history_len;
            free(history[history_len]);
            errno = ENOENT;
            return NULL;
        }
        break;
    case CTRL_T: // Ctrl-t, swaps current character with previous
        if (l->pos > 0 && l->pos < l->len) {
            const char aux = l->buf[l->pos - 1];
            l->buf[l->pos - 1] = l->buf[l->pos];
            l->buf[l->pos] = aux;
            if (l->pos != l->len - 1) {
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
        comlinClearScreen();
        refreshLine(l);
        break;
    case CTRL_W: // Ctrl+w, delete previous word
        comlinEditDeletePrevWord(l);
        break;
    }
    return comlinEditMore;
}

void
comlinEditStop(struct comlinState *l)
{
    if (!isatty(l->ifd)) {
        return;
    }
    disableRawMode(l->ifd);
    printf("\n");
}

/* This just implements a blocking loop for the multiplexed API.
 * In many applications that are not event-driven, we can just call
 * the blocking comlin API, wait for the user to complete the editing
 * and return the buffer. */
static char *
comlinBlockingEdit(int stdin_fd,
                   int stdout_fd,
                   char *buf,
                   size_t buflen,
                   const char *prompt)
{
    struct comlinState l;

    // Editing without a buffer is invalid
    if (buflen == 0) {
        errno = EINVAL;
        return NULL;
    }

    comlinEditStart(&l, stdin_fd, stdout_fd, buf, buflen, prompt);
    char *res = NULL;
    while ((res = comlinEditFeed(&l)) == comlinEditMore) {
    }
    comlinEditStop(&l);
    return res;
}

void
comlinPrintKeyCodes(void)
{
    char quit[4];

    printf("Comlin key codes debugging mode.\n"
           "Press keys to see scan codes. Type 'quit' at any time to exit.\n");
    if (enableRawMode(STDIN_FILENO) == -1) {
        return;
    }
    memset(quit, ' ', 4);
    while (1) {
        char c = '\0';
        const int nread = read(STDIN_FILENO, &c, 1);
        if (nread <= 0) {
            continue;
        }
        memmove(quit, quit + 1, sizeof(quit) - 1); // Shift string to left
        quit[sizeof(quit) - 1] = c; // Insert current char on the right
        if (memcmp(quit, "quit", sizeof(quit)) == 0) {
            break;
        }

        printf("'%c' %02x (%d) (type quit to exit)\n",
               isprint(c) ? c : '?',
               (unsigned)c,
               (int)c);
        printf("\r"); // Go left edge manually, we are in raw mode
        fflush(stdout);
    }
    disableRawMode(STDIN_FILENO);
}

/* This function is called when comlin() is called with the standard
 * input file descriptor not attached to a TTY. So for example when the
 * program using comlin is called in pipe or with a file redirected
 * to its standard input. In this case, we want to be able to return the
 * line regardless of its length (by default we are limited to 4k). */
static char *
comlinNoTTY(void)
{
    char *line = NULL;
    size_t len = 0;
    size_t maxlen = 0;

    while (1) {
        if (len == maxlen) {
            if (maxlen == 0) {
                maxlen = 16;
            }
            maxlen *= 2;
            char *oldval = line;
            line = (char *)realloc(line, maxlen);
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

char *
comlin(const char *prompt)
{
    char buf[COMLIN_MAX_LINE];

    if (!isatty(STDIN_FILENO)) {
        /* Not a tty: read from file / pipe. In this mode we don't want any
         * limit to the line size, so we call a function to handle that. */
        return comlinNoTTY();
    }

    if (isUnsupportedTerm()) {
        printf("%s", prompt);
        fflush(stdout);
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

    char *retval = comlinBlockingEdit(
      STDIN_FILENO, STDOUT_FILENO, buf, COMLIN_MAX_LINE, prompt);
    return retval;
}

void
comlinFree(void *ptr)
{
    if (ptr == comlinEditMore) {
        return; // Protect from API misuse.
    }
    free(ptr);
}

/* ================================ History ================================= */

/* Free the history, but does not reset it. Only used when we have to
 * exit() to avoid memory leaks are reported by valgrind & co. */
static void
freeHistory(void)
{
    if (history) {
        for (int j = 0; j < history_len; ++j) {
            free(history[j]);
        }
        free(history);
    }
}

// At exit we'll try to fix the terminal to the initial conditions
static void
comlinAtExit(void)
{
    disableRawMode(STDIN_FILENO);
    freeHistory();
}

/* Uses a fixed array of char pointers that are shifted (memmoved)
 * when the history max length is reached in order to remove the older
 * entry and make room for the new one, so it is not exactly suitable for huge
 * histories, but will work well for a few hundred of entries.
 *
 * Using a circular buffer is smarter, but a bit more complex to handle. */
int
comlinHistoryAdd(const char *line)
{
    if (history_max_len == 0) {
        return 0;
    }

    // Initialization on first call
    if (history == NULL) {
        history = (char **)malloc(sizeof(char *) * history_max_len);
        if (history == NULL) {
            return 0;
        }
        memset(history, 0, (sizeof(char *) * history_max_len));
    }

    // Don't add duplicated lines
    if (history_len && !strcmp(history[history_len - 1], line)) {
        return 0;
    }

    /* Add an heap allocated copy of the line in the history.
     * If we reached the max length, remove the older line. */
    char *linecopy = strdup(line);
    if (!linecopy) {
        return 0;
    }
    if (history_len == history_max_len) {
        free(history[0]);
        memmove(history, history + 1, sizeof(char *) * (history_max_len - 1));
        --history_len;
    }
    history[history_len] = linecopy;
    ++history_len;
    return 1;
}

int
comlinHistorySetMaxLen(int len)
{
    if (len < 1) {
        return 0;
    }
    if (history) {
        int tocopy = history_len;

        char **new_history = (char **)malloc(sizeof(char *) * len);
        if (new_history == NULL) {
            return 0;
        }

        // If we can't copy everything, free the elements we'll not use
        if (len < tocopy) {
            for (int j = 0; j < tocopy - len; ++j) {
                free(history[j]);
            }
            tocopy = len;
        }
        memset(new_history, 0, sizeof(char *) * len);
        memcpy(new_history,
               history + (history_len - tocopy),
               sizeof(char *) * tocopy);
        free(history);
        history = new_history;
    }
    history_max_len = len;
    if (history_len > history_max_len) {
        history_len = history_max_len;
    }
    return 1;
}

int
comlinHistorySave(const char *filename)
{
    mode_t old_umask = umask(S_IXUSR | S_IRWXG | S_IRWXO);
    FILE *fp = fopen(filename, "w");
    umask(old_umask);
    if (fp == NULL) {
        return -1;
    }
    chmod(filename, S_IRUSR | S_IWUSR);
    for (int j = 0; j < history_len; ++j) {
        fprintf(fp, "%s\n", history[j]);
    }
    fclose(fp);
    return 0;
}

int
comlinHistoryLoad(const char *filename)
{
    FILE *fp = fopen(filename, "r");
    char buf[COMLIN_MAX_LINE];

    if (fp == NULL) {
        return -1;
    }

    while (fgets(buf, COMLIN_MAX_LINE, fp) != NULL) {
        char *p = strchr(buf, '\r');
        if (!p) {
            p = strchr(buf, '\n');
        }
        if (p) {
            *p = '\0';
        }
        comlinHistoryAdd(buf);
    }
    fclose(fp);
    return 0;
}
