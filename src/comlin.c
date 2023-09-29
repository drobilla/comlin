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
 * - Add Win32 support.
 */

#include "comlin/comlin.h"

#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A resizable buffer that contains a string
typedef struct {
    char* data;    ///< Pointer to string buffer
    size_t length; ///< Length of string (not greater than size)
    size_t size;   ///< Size of data
} StringBuf;

struct ComlinStateImpl {
    // Completion
    ComlinCompletionCallback* completion_callback; ///< Get completions

    // Terminal session state
    int ifd;       ///< Terminal stdin file descriptor
    int ofd;       ///< Terminal stdout file descriptor
    size_t cols;   ///< Number of columns in terminal
    bool maskmode; ///< Show asterisks instead of input (for passwords)
    bool rawmode;  ///< Terminal is currently in raw mode
    bool mlmode;   ///< Multi-line mode (default is single line)
    bool dumb;     ///< True if terminal is unsupported (no features)

    // History
    size_t history_max_len; ///< Maximum number of history entries to keep
    size_t history_len;     ///< Number of history entries
    char** history;         ///< History entries

    // Line editing state
    struct termios cooked; ///< Terminal settings before raw mode
    StringBuf buf;         ///< Editing line buffer
    char const* prompt;    ///< Prompt to display
    size_t plen;           ///< Prompt length
    size_t pos;            ///< Current cursor position
    size_t history_index;  ///< The history index we're currently editing
    bool in_completion;    ///< Currently doing a completion
    size_t completion_idx; ///< Index of next completion to propose

    // Multi-line refresh state
    size_t oldpos;  ///< Previous refresh cursor position
    size_t oldrows; ///< Rows used by last refreshed line (multi-line)
};

static char const* const unsupported_term[] = {"dumb", "cons25", "emacs", NULL};

static void
buf_append(StringBuf* buf, char const* s, size_t len);

static void
refresh_line_with_completion(ComlinState* ls,
                             ComlinCompletions const* lc,
                             unsigned flags);

static void
refresh_line_with_flags(ComlinState* l, unsigned flags);

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

typedef unsigned ComlinRefreshFlags;

static const ComlinRefreshFlags REFRESH_CLEAN = 1U << 0U;
static const ComlinRefreshFlags REFRESH_WRITE = 1U << 1U;
static const ComlinRefreshFlags REFRESH_ALL = REFRESH_CLEAN | REFRESH_WRITE;

static void
refresh_line(ComlinState* l);

/* ======================= Low level terminal handling ====================== */

void
comlin_mask_mode_enable(ComlinState* const comlin)
{
    comlin->maskmode = true;
}

void
comlin_mask_mode_disable(ComlinState* const comlin)
{
    comlin->maskmode = false;
}

void
comlin_set_multi_line(ComlinState* const comlin, bool const ml)
{
    comlin->mlmode = ml;
}

/* Return true if the terminal name is in the list of terminals we know are
 * not able to understand basic escape sequences. */
static bool
is_unsupported_term(char const* const term)
{
    if (term) {
        for (unsigned i = 0U; unsupported_term[i]; ++i) {
            if (!strcasecmp(term, unsupported_term[i])) {
                return true;
            }
        }
    }

    return false;
}

static ssize_t
read_full(int const fd, char* const buf, size_t const count)
{
    size_t offset = 0U;
    while (offset < count) {
        ssize_t const r = read(fd, buf + offset, count - offset);
        if (r <= 0) {
            return -1;
        }

        offset += (size_t)r;
    }

    return (ssize_t)count;
}

static ComlinStatus
write_string(int const fd, char const* const buf, size_t const count)
{
    size_t offset = 0U;
    while (offset < count) {
        ssize_t const r = write(fd, buf + offset, count - offset);
        if (r < 0) {
            return COMLIN_BAD_WRITE;
        }

        offset += (size_t)r;
    }

    return COMLIN_SUCCESS;
}

/// Set terminal to raw input mode and preserve the original settings
static ComlinStatus
enable_raw_mode(ComlinState* const state)
{
    if (!isatty(state->ifd)) {
        return COMLIN_SUCCESS;
    }

    // Save original terminal attributes
    if (tcgetattr(state->ifd, &state->cooked) == -1) {
        return COMLIN_BAD_TERMINAL;
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
    int const rc = tcsetattr(state->ifd, TCSAFLUSH, &raw);
    state->rawmode = !rc;
    return rc ? COMLIN_BAD_TERMINAL : COMLIN_SUCCESS;
}

static ComlinStatus
disableRawMode(ComlinState* const state)
{
    if (state->rawmode) {
        if (tcsetattr(state->ifd, TCSAFLUSH, &state->cooked) == -1) {
            return COMLIN_BAD_TERMINAL;
        }

        state->rawmode = false;
    }

    return COMLIN_SUCCESS;
}

/* Use the ESC [6n escape sequence to query the horizontal cursor position
 * and return it. On error -1 is returned, on success the position of the
 * cursor. */
static int
get_cursor_position(int const ifd, int const ofd)
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
get_columns(int const ifd, int const ofd)
{
    struct winsize ws = {24U, 80U, 640U, 480U};

    if (isatty(ofd) && (ioctl(ofd, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)) {
        // ioctl() failed. Try to query the terminal itself

        // Get the initial position so we can restore it later
        int start = get_cursor_position(ifd, ofd);
        if (start == -1) {
            goto failed;
        }

        // Go to right margin and get position
        if (write_string(ofd, "\x1B[999C", 6)) {
            goto failed;
        }
        int cols = get_cursor_position(ifd, ofd);
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
comlin_clear_screen(ComlinState* const state)
{
    if (write_string(state->ofd, "\x1B[H\x1B[2J", 7)) {
        // Failed to clear screen, oh well
    }
}

/* Beep, used for completion when there is nothing to complete or when all
 * the choices were already shown. */
static void
comlin_beep(ComlinState const* const state)
{
    if (write(state->ofd, "\x07", 1) != 1) {
        // Failed to write ASCII BEL, oh well
    }
}

/* ============================== Completion ================================ */

// Free a list of completion option populated by comlinAddCompletion()
static void
free_completions(ComlinCompletions* const lc)
{
    for (size_t i = 0U; i < lc->len; ++i) {
        free(lc->cvec[i]);
    }
    if (lc->cvec != NULL) {
        free(lc->cvec);
    }
}

// Show the current line with the proposed completion
static void
refresh_line_with_completion(ComlinState* const ls,
                             ComlinCompletions const* const lc,
                             unsigned const flags)
{
    // Show the edited line with completion if possible, or just refresh
    if (ls->completion_idx < lc->len) {
        size_t const saved_pos = ls->pos;
        StringBuf const saved_buf = ls->buf;
        ls->buf.data = lc->cvec[ls->completion_idx];
        ls->pos = ls->buf.length = strlen(ls->buf.data);
        refresh_line_with_flags(ls, flags);
        ls->buf = saved_buf;
        ls->pos = saved_pos;
    } else {
        refresh_line_with_flags(ls, flags);
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
complete_line(ComlinState* const ls, char const keypressed)
{
    ComlinCompletions lc = {0, NULL};
    char c = keypressed;

    if (ls->buf.length) {
        ls->completion_callback(ls->buf.data, &lc);
    }

    if (lc.len == 0) {
        comlin_beep(ls);
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
                    comlin_beep(ls);
                }
            }
            c = 0;
            break;
        case ESC: // Escape
            // Re-show original buffer
            if (ls->completion_idx < lc.len) {
                refresh_line(ls);
            }
            ls->in_completion = false;
            c = 0;
            break;
        default:
            // Update buffer and return
            if (ls->completion_idx < lc.len) {
                ls->pos = strlen(lc.cvec[ls->completion_idx]);
                ls->buf.length = 0U;
                buf_append(&ls->buf, lc.cvec[ls->completion_idx], ls->pos);
            }
            ls->in_completion = false;
            break;
        }

        // Show completion or original buffer
        if (ls->in_completion && ls->completion_idx < lc.len) {
            refresh_line_with_completion(ls, &lc, REFRESH_ALL);
        } else {
            refresh_line(ls);
        }
    }

    free_completions(&lc);
    return c; // Return last read character
}

void
comlin_set_completion_callback(ComlinState* const state,
                               ComlinCompletionCallback* const fn)
{
    state->completion_callback = fn;
}

void
comlin_add_completion(ComlinCompletions* const lc, char const* const str)
{
    size_t const len = strlen(str);

    char* const copy = (char*)malloc(len + 1);
    if (!copy) {
        return;
    }

    memcpy(copy, str, len + 1);
    char** const cvec =
      (char**)realloc(lc->cvec, sizeof(char*) * (lc->len + 1));
    if (!cvec) {
        free(copy);
        return;
    }
    lc->cvec = cvec;
    lc->cvec[lc->len++] = copy;
}

/* =========================== Line editing ================================= */

static void
buf_append(StringBuf* const buf, char const* const s, size_t const len)
{
    assert(s);

    size_t const new_length = buf->length + len;
    size_t const needed_size = new_length + 1U;
    if (needed_size > buf->size) {
        char* const new_data = (char*)realloc(buf->data, needed_size);
        if (!new_data) {
            return;
        }

        buf->data = new_data;
        buf->size = needed_size;
    }

    assert(buf->data);
    memcpy(buf->data + buf->length, s, len);
    buf->data[new_length] = '\0';
    buf->length = new_length;
}

static void
buf_free(StringBuf* const buf)
{
    free(buf->data);
}

static void
append_line_text(StringBuf* const buf,
                 char const* const text,
                 size_t const length,
                 bool const masked)
{
    if (masked) {
        for (size_t i = 0U; i < length; ++i) {
            buf_append(buf, "*", 1);
        }
    } else {
        buf_append(buf, text, length);
    }
}

/* Single line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal.  Flags control
 * whether the line is cleared, written, or both.
 */
static void
refresh_single_line(ComlinState const* const l, ComlinRefreshFlags const flags)
{
    int const fd = l->ofd;

    char* buf = l->buf.data;
    size_t len = l->buf.length;
    size_t pos = l->pos;
    while (l->plen + pos >= l->cols) {
        ++buf;
        --len;
        --pos;
    }
    while (l->plen + len > l->cols) {
        --len;
    }

    // We'll build the update here, then send it all in a single write
    StringBuf update = {NULL, 0U, 0U};
    char seq[64] = {0};

    // Move cursor to left edge
    buf_append(&update, "\r", 1);

    if (l->buf.data && (flags & REFRESH_WRITE)) {
        // Write the prompt and the current buffer content
        buf_append(&update, l->prompt, l->plen);
        append_line_text(&update, buf, len, l->maskmode);
    }

    // Erase to right
    buf_append(&update, "\x1B[0K", 4);

    if (l->buf.data && (flags & REFRESH_WRITE)) {
        // Move cursor to original position
        snprintf(seq, sizeof(seq), "\r\x1B[%dC", (int)(pos + l->plen));
        buf_append(&update, seq, strlen(seq));
    }

    if (write_string(fd, update.data, update.length)) {
        // Failed to write to terminal, this is bad and should be reported...
    }

    buf_free(&update);
}

/* Multi line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content, cursor
 * position, and number of columns of the terminal.  The current terminal line
 * is always effectively cleared, but the line is only written if
 * #REFRESH_WRITE is given.
 */
static void
refresh_multi_line(ComlinState* const l, ComlinRefreshFlags const flags)
{
    size_t rows = (l->plen + l->buf.length + l->cols - 1U) / l->cols;
    size_t const rpos = (l->plen + l->oldpos + l->cols) / l->cols;
    size_t const old_rows = l->oldrows;
    int const fd = l->ofd;

    l->oldrows = rows;

    // We'll build the update here, then send it all in a single write
    StringBuf update = {NULL, 0U, 0U};
    char seq[64] = {0};

    // First clear all the old used rows
    if (flags & REFRESH_CLEAN) {
        // Go to the last row
        if (old_rows > rpos) {
            snprintf(seq, 64, "\x1B[%zuB", old_rows - rpos);
            buf_append(&update, seq, strlen(seq));
        }

        // For each row, clear it, then move up
        for (size_t j = 1U; j < old_rows; ++j) {
            buf_append(&update, "\r\x1B[0K\x1B[1A", 9U);
        }
    }

    if (flags & REFRESH_WRITE) {
        // Move cursor to left edge
        buf_append(&update, "\r", 1);

        // Write the prompt and the current buffer content
        buf_append(&update, l->prompt, l->plen);
        append_line_text(&update, l->buf.data, l->buf.length, l->maskmode);

        // Erase to right
        buf_append(&update, "\x1B[0K", 4U);

        /* If we are at the very end of the screen with our prompt, we need to
         * emit a newline and move the prompt to the first column. */
        if (l->pos && l->pos == l->buf.length &&
            (l->pos + l->plen) % l->cols == 0) {
            buf_append(&update, "\n\r", 2);
            ++rows;
            if (rows > l->oldrows) {
                l->oldrows = rows;
            }
        }

        // Move cursor to right position
        size_t const rpos2 =
          (l->plen + l->pos + l->cols) / l->cols; // Current cursor relative row

        // Go up till we reach the expected position
        if (rows > rpos2) {
            snprintf(seq, 64, "\x1B[%zuA", rows - rpos2);
            buf_append(&update, seq, strlen(seq));
        }

        // Set column
        size_t const col = (l->plen + l->pos) % l->cols;
        if (col) {
            snprintf(seq, 64, "\r\x1B[%zuC", col);
        } else {
            snprintf(seq, 64, "\r");
        }
        buf_append(&update, seq, strlen(seq));
    }

    l->oldpos = l->pos;

    if (write_string(fd, update.data, update.length)) {
        // Failed to write to terminal, this is bad and should be reported...
    }

    buf_free(&update);
}

/* Calls the two low level functions refreshSingleLine() or
 * refreshMultiLine() according to the selected mode. */
static void
refresh_line_with_flags(ComlinState* const l, ComlinRefreshFlags const flags)
{
    if (l->mlmode) {
        refresh_multi_line(l, flags);
    } else {
        refresh_single_line(l, flags);
    }
}

// Utility function to avoid specifying REFRESH_ALL all the times
static void
refresh_line(ComlinState* const l)
{
    refresh_line_with_flags(l, REFRESH_ALL);
}

void
comlin_hide(ComlinState* const l)
{
    if (l->mlmode) {
        refresh_multi_line(l, REFRESH_CLEAN);
    } else {
        refresh_single_line(l, REFRESH_CLEAN);
    }
}

void
comlin_show(ComlinState* const l)
{
    if (l->in_completion && l->buf.length) {
        ComlinCompletions completions = {0U, NULL};
        l->completion_callback(l->buf.data, &completions);
        refresh_line_with_completion(l, &completions, REFRESH_WRITE);
    } else {
        refresh_line_with_flags(l, REFRESH_WRITE);
    }
}

/* Insert the character 'c' at cursor current position.
 *
 * On error writing to the terminal -1 is returned, otherwise 0. */
static ComlinStatus
comlin_edit_insert(ComlinState* const l, char const c)
{
    if (l->buf.length == l->pos) {
        // Insert at end of line
        buf_append(&l->buf, &c, 1U);
        ++l->pos;
        if ((!l->mlmode || l->oldrows <= 1U) &&
            l->plen + l->buf.length < l->cols) {
            // Avoid a full update of the line in the trivial case
            char const d = (char)(l->maskmode ? '*' : c);
            return write(l->ofd, &d, 1) == 1 ? COMLIN_READING
                                             : COMLIN_BAD_WRITE;
        }

        refresh_line(l);

    } else {
        // Insert in middle of line
        buf_append(&l->buf, " ", 1U); // Allocate space for one extra character
        memmove(l->buf.data + l->pos + 1,
                l->buf.data + l->pos,
                l->buf.length - l->pos);
        l->buf.data[l->pos] = c;
        ++l->pos;
        refresh_line(l);
    }

    return COMLIN_READING;
}

// Move cursor on the left
static void
comlin_edit_move_left(ComlinState* const l)
{
    if (l->pos > 0) {
        --l->pos;
        refresh_line(l);
    }
}

// Move cursor on the right
static void
comlin_edit_move_right(ComlinState* const l)
{
    if (l->pos != l->buf.length) {
        ++l->pos;
        refresh_line(l);
    }
}

// Move cursor to the start of the line
static void
comlin_edit_move_home(ComlinState* const l)
{
    if (l->pos != 0) {
        l->pos = 0;
        refresh_line(l);
    }
}

// Move cursor to the end of the line
static void
comlin_edit_move_end(ComlinState* const l)
{
    if (l->pos != l->buf.length) {
        l->pos = l->buf.length;
        refresh_line(l);
    }
}

static void
comlin_edit_transpose(ComlinState* const state)
{
    if (state->pos > 0U && state->pos < state->buf.length) {
        char const aux = state->buf.data[state->pos - 1];
        state->buf.data[state->pos - 1] = state->buf.data[state->pos];
        state->buf.data[state->pos] = aux;
        if (state->pos != state->buf.length - 1U) {
            ++state->pos;
        }
        refresh_line(state);
    }
}

typedef enum {
    COMLIN_HISTORY_NEXT,
    COMLIN_HISTORY_PREV,
} ComlinHistoryDirection;

// Substitute the currently edited line with the next or previous history entry
static void
comlin_edit_history_next(ComlinState* const l, ComlinHistoryDirection const dir)
{
    if (l->history_len > 1U) {
        // Update the current history entry before overwriting it with the next
        free(l->history[l->history_len - 1U - l->history_index]);
        l->history[l->history_len - 1U - l->history_index] =
          strdup(l->buf.data);

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

        l->pos = strlen(l->history[l->history_len - 1U - l->history_index]);
        l->buf.length = 0U;
        buf_append(
          &l->buf, l->history[l->history_len - 1U - l->history_index], l->pos);
        l->buf.data[l->pos] = '\0';
        refresh_line(l);
    }
}

static void
comlin_edit_history_pop(ComlinState* const state)
{
    --state->history_len;
    free(state->history[state->history_len]);
    state->history_index = 0U;
}

/* Delete the character at the right of the cursor without altering the cursor
 * position. Basically this is what happens with the "Delete" keyboard key. */
static void
comlin_edit_delete(ComlinState* const l)
{
    if (l->buf.length && l->pos < l->buf.length) {
        memmove(l->buf.data + l->pos,
                l->buf.data + l->pos + 1U,
                l->buf.length - l->pos - 1U);
        --l->buf.length;
        l->buf.data[l->buf.length] = '\0';
        refresh_line(l);
    }
}

// Backspace implementation
static void
comlin_edit_backspace(ComlinState* const l)
{
    if (l->pos && l->buf.length) {
        memmove(l->buf.data + l->pos - 1U,
                l->buf.data + l->pos,
                l->buf.length - l->pos);
        --l->pos;
        --l->buf.length;
        l->buf.data[l->buf.length] = '\0';
        refresh_line(l);
    }
}

/* Delete the previous word, maintaining the cursor at the start of the
 * current word. */
static void
comlin_edit_delete_prev_word(ComlinState* const l)
{
    size_t const old_pos = l->pos;

    while (l->pos > 0 && l->buf.data[l->pos - 1U] == ' ') {
        --l->pos;
    }
    while (l->pos > 0 && l->buf.data[l->pos - 1U] != ' ') {
        --l->pos;
    }
    size_t const diff = old_pos - l->pos;
    memmove(l->buf.data + l->pos,
            l->buf.data + old_pos,
            l->buf.length + 1U - old_pos);
    l->buf.length -= diff;
    refresh_line(l);
}

static void
comlin_edit_clear_line(ComlinState* const l)
{
    l->buf.data[0] = '\0';
    l->pos = l->buf.length = 0;
    refresh_line(l);
}

static void
comlin_edit_clear_to_end_of_line(ComlinState* const l)
{
    if (l->pos < l->buf.length) {
        l->buf.data[l->pos] = '\0';
        l->buf.length = l->pos;
        refresh_line(l);
    }
}

ComlinState*
comlin_new_state(int const in_fd,
                 int const out_fd,
                 char const* const term,
                 size_t const max_history_len)
{
    ComlinState* const l = (ComlinState*)calloc(1, sizeof(ComlinState));
    if (!l) {
        return NULL;
    }

    l->ifd = in_fd;
    l->ofd = out_fd;
    l->dumb = is_unsupported_term(term);
    l->history_max_len = max_history_len;
    l->cols = (size_t)get_columns(in_fd, out_fd);
    l->buf.data = (char*)calloc(1, l->cols);
    l->buf.size = l->cols;

    return l;
}

ComlinStatus
comlin_edit_start(ComlinState* const l, char const* const prompt)
{
    // Enter raw mode
    ComlinStatus const st = enable_raw_mode(l);
    if (st) {
        return st;
    }

    // Reset line state
    l->buf.data[0] = '\0';
    l->pos = 0U;
    l->oldpos = 0U;
    l->buf.length = 0U;
    l->oldrows = 0U;

    /* The latest history entry is always our current buffer, that
     * initially is just an empty string. */
    comlin_history_add(l, "");

    // Set and write prompt
    l->prompt = prompt;
    l->plen = strlen(prompt);
    return write_string(l->ofd, l->prompt, l->plen);
}

static ComlinStatus
comlin_edit_read_dumb(ComlinState* const l, char const c)
{
    switch (c) {
    case '\n':
    case '\r':
        return COMLIN_SUCCESS;
    case CTRL_C:
        return COMLIN_INTERRUPTED;
    case CTRL_D:
        return COMLIN_END;
    default:
        break;
    }

    write(l->ofd, &c, 1U);
    buf_append(&l->buf, &c, 1U);
    return COMLIN_READING;
}

ComlinStatus
comlin_edit_feed(ComlinState* const l)
{
    // Read the next character
    char c = '\0';
    ssize_t const nread = read(l->ifd, &c, 1);
    if (nread < 0) {
        return COMLIN_BAD_READ;
    }
    if (nread == 0) {
        return COMLIN_END;
    }

    if (l->dumb) {
        return comlin_edit_read_dumb(l, c);
    }

    if ((l->in_completion || c == TAB) && l->completion_callback) {
        // Try to autocomplete
        c = complete_line(l, c);
        if (c < 0) {
            return COMLIN_BAD_READ;
        }
        if (c == 0) {
            return COMLIN_READING;
        }
    }

    char seq[3] = {'\0', '\0', '\0'};
    switch (c) {
    case '\n':
    case ENTER: // Enter
        comlin_edit_history_pop(l);
        if (l->mlmode) {
            comlin_edit_move_end(l);
        }
        l->buf.data[l->buf.length] = '\0';
        return COMLIN_SUCCESS; // Command ready for access with comlinText()
    case CTRL_C:               // Ctrl-c
        return COMLIN_INTERRUPTED;
    case BACKSPACE: // Backspace
    case CTRL_H:    // Ctrl-h
        comlin_edit_backspace(l);
        break;
    case CTRL_D: /* ctrl-d, remove char at right of cursor, or if the
                    line is empty, act as end-of-file. */
        if (l->buf.length > 0) {
            comlin_edit_delete(l);
        } else {
            comlin_edit_history_pop(l);
            return COMLIN_END;
        }
        break;
    case CTRL_T: // Ctrl-t, swaps current character with previous
        comlin_edit_transpose(l);
        break;
    case CTRL_B: // Ctrl-b
        comlin_edit_move_left(l);
        break;
    case CTRL_F: // Ctrl-f
        comlin_edit_move_right(l);
        break;
    case CTRL_P: // Ctrl-p
        comlin_edit_history_next(l, COMLIN_HISTORY_PREV);
        break;
    case CTRL_N: // Ctrl-n
        comlin_edit_history_next(l, COMLIN_HISTORY_NEXT);
        break;
    case ESC: // Escape sequence
        // Read the next two bytes representing the escape sequence
        if (read_full(l->ifd, seq, 2) != 2) {
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
                        comlin_edit_delete(l);
                        break;
                    }
                }
            } else {
                switch (seq[1]) {
                case 'A': // Up
                    comlin_edit_history_next(l, COMLIN_HISTORY_PREV);
                    break;
                case 'B': // Down
                    comlin_edit_history_next(l, COMLIN_HISTORY_NEXT);
                    break;
                case 'C': // Right
                    comlin_edit_move_right(l);
                    break;
                case 'D': // Left
                    comlin_edit_move_left(l);
                    break;
                case 'H': // Home
                    comlin_edit_move_home(l);
                    break;
                case 'F': // End
                    comlin_edit_move_end(l);
                    break;
                }
            }
        }

        // ESC O sequences
        else if (seq[0] == 'O') {
            switch (seq[1]) {
            case 'H': // Home
                comlin_edit_move_home(l);
                break;
            case 'F': // End
                comlin_edit_move_end(l);
                break;
            }
        }
        break;
    case CTRL_U: // Ctrl+u, delete the whole line
        comlin_edit_clear_line(l);
        break;
    case CTRL_K: // Ctrl+k, delete from current to end of line
        comlin_edit_clear_to_end_of_line(l);
        break;
    case CTRL_A: // Ctrl+a, go to the start of the line
        comlin_edit_move_home(l);
        break;
    case CTRL_E: // Ctrl+e, go to the end of the line
        comlin_edit_move_end(l);
        break;
    case CTRL_L: // Ctrl+l, clear screen
        comlin_clear_screen(l);
        refresh_line(l);
        break;
    case CTRL_W: // Ctrl+w, delete previous word
        comlin_edit_delete_prev_word(l);
        break;
    default:
        if (c >= 0x20) {
            return comlin_edit_insert(l, c);
        }
        break;
    }
    return COMLIN_READING;
}

ComlinStatus
comlin_edit_stop(ComlinState* const l)
{
    ComlinStatus const st = disableRawMode(l);

    return st ? st : write_string(l->ofd, "\n", 1);
}

char const*
comlin_text(ComlinState const* const l)
{
    return l->buf.data;
}

ComlinStatus
comlin_read_line(ComlinState* const state, char const* const prompt)
{
    state->prompt = prompt;
    state->plen = strlen(prompt);

    ComlinStatus st0 = comlin_edit_start(state, prompt);
    ComlinStatus st1 = COMLIN_SUCCESS;
    if (!st0) {
        do {
            st0 = comlin_edit_feed(state);
        } while (st0 == COMLIN_READING);

        st1 = comlin_edit_stop(state);
    }
    return st0 ? st0 : st1;
}

/* ================================ History ================================= */

/* Uses a fixed array of char pointers that are shifted (memmoved)
 * when the history max length is reached in order to remove the older
 * entry and make room for the new one, so it is not exactly suitable for huge
 * histories, but will work well for a few hundred of entries.
 *
 * Using a circular buffer is smarter, but a bit more complex to handle. */
ComlinStatus
comlin_history_add(ComlinState* const state, char const* const line)
{
    if (state->history_max_len == 0) {
        return COMLIN_SUCCESS;
    }

    // Initialization on first call
    if (!state->history) {
        state->history = (char**)malloc(sizeof(char*) * state->history_max_len);
        if (!state->history) {
            return COMLIN_NO_MEMORY;
        }
        memset(state->history, 0, (sizeof(char*) * state->history_max_len));
    }

    // Don't add duplicated lines
    if (state->history_len &&
        !strcmp(state->history[state->history_len - 1U], line)) {
        return COMLIN_SUCCESS;
    }

    // Add an heap allocated copy of the line in the history
    char* const linecopy = strdup(line);
    if (!linecopy) {
        return COMLIN_NO_MEMORY;
    }

    // If we reached the max length, remove the older line
    if (state->history_len == state->history_max_len) {
        free(state->history[0]);
        memmove(state->history,
                state->history + 1,
                sizeof(char*) * (state->history_max_len - 1U));
        --state->history_len;
    }

    state->history[state->history_len] = linecopy;
    ++state->history_len;
    return COMLIN_SUCCESS;
}

ComlinStatus
comlin_history_save(ComlinState const* const state, char const* const filename)
{
    mode_t const old_umask = umask(S_IXUSR | S_IRWXG | S_IRWXO);
    FILE* const fp = fopen(filename, "w");
    umask(old_umask);
    if (!fp) {
        return COMLIN_NO_FILE;
    }
    chmod(filename, S_IRUSR | S_IWUSR);
    for (size_t j = 0U; j < state->history_len; ++j) {
        fprintf(fp, "%s\n", state->history[j]);
    }
    fclose(fp);
    return COMLIN_SUCCESS;
}

ComlinStatus
comlin_history_load(ComlinState* const state, char const* const filename)
{
#define COMLIN_MAX_LINE 4096

    FILE* const fp = fopen(filename, "r");
    char buf[COMLIN_MAX_LINE];

    if (!fp) {
        return COMLIN_NO_FILE;
    }

    while (fgets(buf, COMLIN_MAX_LINE, fp) != NULL) {
        char* p = strchr(buf, '\r');
        if (!p) {
            p = strchr(buf, '\n');
        }
        if (p) {
            *p = '\0';
        }
        comlin_history_add(state, buf);
    }
    fclose(fp);
    return COMLIN_SUCCESS;

#undef COMLIN_MAX_LINE
}

void
comlin_free_state(ComlinState* const state)
{
    // Free history
    for (size_t j = 0U; j < state->history_len; ++j) {
        free(state->history[j]);
    }
    free(state->history);

    // Disable raw mode if it was enabled by comlinNew
    disableRawMode(state);

    free(state->buf.data);
    free(state);
}
