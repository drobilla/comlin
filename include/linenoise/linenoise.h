// Copyright 2010-2023 Salvatore Sanfilippo <antirez@gmail.com>
// Copyright 2010-2013 Pieter Noordhuis <pcnoordhuis@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause

#ifndef LINENOISE_LINENOISE_H
#define LINENOISE_LINENOISE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
   @defgroup linenoise Linenoise C API
   @{
*/

/// Sentinel return string for when the user is still editing the line
extern char *linenoiseEditMore;

/** The state during line editing.
 *
 * This is passed to functions implementing specific editing functionalities.
 */
struct linenoiseState {
    int in_completion; /**< The user pressed TAB and we are now in completion
                        * mode, so input is handled by completeLine(). */

    size_t completion_idx; ///< Index of next completion to propose

    int ifd;            ///< Terminal stdin file descriptor
    int ofd;            ///< Terminal stdout file descriptor
    char *buf;          ///< Edited line buffer
    size_t buflen;      ///< Edited line buffer size
    const char *prompt; ///< Prompt to display
    size_t plen;        ///< Prompt length
    size_t pos;         ///< Current cursor position
    size_t oldpos;      ///< Previous refresh cursor position
    size_t len;         ///< Current edited line length
    size_t cols;        ///< Number of columns in terminal
    size_t oldrows;     ///< Rows used by last refreshed line (multi-line mode)
    int history_index;  ///< The history index we are currently editing
};

/** A sequence of applicable completions.
 *
 * This is passed to the completion callback, which can add completions to it
 * with #linenoiseAddCompletion.
 */
typedef struct linenoiseCompletions {
    size_t len;  ///< Number of elements in cvec
    char **cvec; ///< Array of string pointers
} linenoiseCompletions;

/**
   @defgroup linenoise_non_blocking Non-blocking API
   @{
*/

/** Start a non-blocking command line read.
 *
 * This will:
 *
 * 1. Initialize the linenoise state passed by the user.
 * 2. Put the terminal in RAW mode.
 * 3. Show the prompt.
 * 4. Return control to the user, that will have to call #linenoiseEditFeed
 *    each time there is some data arriving in the standard input.
 *
 * The user can also call #linenoiseHide and #linenoiseShow if it is required
 * to show some input arriving asynchronously, without mixing it with the
 * currently edited line.
 *
 * When #linenoiseEditFeed returns non-NULL, the user finished with the line
 * editing session (pressed enter CTRL-D/C): in this case the caller needs to
 * call #linenoiseEditStop to put back the terminal in normal mode.  This won't
 * destroy the buffer, as long as the #linenoiseState is still valid in the
 * context of the caller.
 *
 * @return 0 on success, or -1 if writing to standard output fails.
 */
int linenoiseEditStart(struct linenoiseState *l,
                       int stdin_fd,
                       int stdout_fd,
                       char *buf,
                       size_t buflen,
                       const char *prompt);

/** This function is part of the multiplexed API of linenoise, see the top
 * comment on #linenoiseEditStart for more information.  Call this function
 * each time there is some data to read from the standard input file
 * descriptor.  In the case of blocking operations, this function can just be
 * called in a loop, and block.
 *
 * The function returns #linenoiseEditMore to signal that line editing is still
 * in progress, that is, the user didn't yet pressed enter / CTRL-D.  Otherwise
 * the function returns the pointer to the heap-allocated buffer with the
 * edited line, that the user should free with #linenoiseFree.
 *
 * On special conditions, NULL is returned and errno is set to `EAGAIN` if the
 * user pressed Ctrl-C, `ENOENT` if the user pressed Ctrl-D, or some other
 * error number on an I/O error.
 */
char *linenoiseEditFeed(struct linenoiseState *l);

/** Finish editing a line.
 *
 * This should be called when #linenoiseEditFeed returns something a finished
 * command string.  At that point, the user input is in the buffer, and we can
 * restore the terminal to normal mode.
 */
void linenoiseEditStop(struct linenoiseState *l);

/// Hide the current line, when using the multiplexing API
void linenoiseHide(struct linenoiseState *l);

/// Show the current line, when using the multiplexing API
void linenoiseShow(struct linenoiseState *l);

/**
   @}
   @defgroup linenoise_blocking Blocking API
   @{
*/

/** Simple high-level entry point.
 *
 * This checks if the terminal has basic capabilities, and later either calls
 * the line editing function or uses dummy fgets() so that you will be able to
 * type something even in the most desperate of the conditions.
 *
 * @return A newly allocated command string that must be freed with
 * #linenoiseFree.
 */
char *linenoise(const char *prompt);

/// Free a command string returned by linenoise()
void linenoiseFree(void *ptr);

/**
   @}
   @defgroup linenoise_completion Completion
   @{
*/

/// Completion callback
typedef void(linenoiseCompletionCallback)(const char *, linenoiseCompletions *);

/// Prompt hint callback
typedef char *(linenoiseHintsCallback)(const char *, int *color, int *bold);

/// Function to free a hint returned by a #linenoiseHintsCallback
typedef void(linenoiseFreeHintsCallback)(void *);

/// Register a callback function to be called for tab-completion
void linenoiseSetCompletionCallback(linenoiseCompletionCallback *fn);

/// Register a callback function to show hints to the right of the prompt
void linenoiseSetHintsCallback(linenoiseHintsCallback *fn);

/// Register a function to free the hints returned by the hints callback
void linenoiseSetFreeHintsCallback(linenoiseFreeHintsCallback *fn);

/** Add completion options for the current input string.
 *
 * This is used by completion callback to add completion options given the
 * input string when the user pressed `TAB`.
 */
void linenoiseAddCompletion(linenoiseCompletions *lc, const char *str);

/**
   @}
   @defgroup linenoise_history History
   @{
*/

/** Add a new entry to the history.
 *
 * The new entry will be added to the history in memory, which can later be
 * saved explicitly with #linenoiseHistorySave.
 */
int linenoiseHistoryAdd(const char *line);

/** Set the maximum length for the history.
 *
 * This function can be called even if there is already some history, it will
 * make sure to retain just the latest 'len' elements if the new history length
 * length is less than the number of items already in the history.
 */
int linenoiseHistorySetMaxLen(int len);

/** Save the history in the specified file.
 *
 * @return 0 on success, otherwise -1.
 */
int linenoiseHistorySave(const char *filename);

/** Load the history from the specified file.
 *
 * @return 0 on success, otherwise -1.
 */
int linenoiseHistoryLoad(const char *filename);

/**
   @}
   @defgroup linenoise_utilities Utilities
   @{
*/

/// Clear the screen
void linenoiseClearScreen(void);

/// Set whether to use multi-line mode
void linenoiseSetMultiLine(int ml);

/** This special mode is used by linenoise in order to print scan codes on
 * screen for debugging / development purposes.  It is implemented by the
 * linenoise_example program using the --keycodes option.
 */
void linenoisePrintKeyCodes(void);

/** Enable "mask mode".
 *
 * When this is enabled, the terminal will hide user input with asterisks.
 * This is useful for passwords and other secrets that shouldn't be *
 * displayed.
 */
void linenoiseMaskModeEnable(void);

/** Disable "mask mode".
 *
 * This will return to showing the user input after it was hidden with
 * linenoiseMaskModeEnable().
 */
void linenoiseMaskModeDisable(void);

/**
   @}
   @}
*/

#ifdef __cplusplus
} // extern "C"
#endif

#endif // LINENOISE_LINENOISE_H
