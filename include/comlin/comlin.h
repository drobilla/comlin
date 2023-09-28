// Copyright 2023 David Robillard <d@drobilla.net>
// Copyright 2010-2023 Salvatore Sanfilippo <antirez@gmail.com>
// Copyright 2010-2013 Pieter Noordhuis <pcnoordhuis@gmail.com>
// SPDX-License-Identifier: BSD-2-Clause

#ifndef COMLIN_COMLIN_H
#define COMLIN_COMLIN_H

#ifndef COMLIN_API
#    if defined(_WIN32) && !defined(COMLIN_STATIC) && defined(COMLIN_INTERNAL)
#        define COMLIN_API __declspec(dllexport)
#    elif defined(_WIN32) && !defined(COMLIN_STATIC)
#        define COMLIN_API __declspec(dllimport)
#    elif defined(__GNUC__)
#        define COMLIN_API __attribute__((visibility("default")))
#    else
#        define COMLIN_API
#    endif
#endif

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
   @defgroup comlin Comlin C API
   @{
*/

/**
   @defgroup comlin_state State
   @{
*/

/** The state of a command line session.
 *
 * This stores all of the state needed by a command line session for a
 * terminal.  A program may have several of these, but only one should exist
 * for a particular terminal.
 */
typedef struct ComlinStateImpl ComlinState;

/** Create a new terminal session.
 *
 * The returned state represents a connection to the terminal.  This may write
 * escape sequences to the terminal to determine its width, but otherwise
 * doesn't cause any output changes.
 *
 * @return A new state that must be freed with comlinFreeState().
 */
COMLIN_API ComlinState*
comlinNewState(int stdin_fd,
               int stdout_fd,
               char* buf,
               size_t buflen,
               const char* prompt);

/** Free a terminal session.
 *
 * If a line edit is still in progress, this will reset the terminal to normal
 * mode, without writing any output (not even a trailing newline).
 */
COMLIN_API void
comlinFreeState(ComlinState* state);

/**
   @}
   @defgroup comlin_non_blocking Non-blocking API
   @{
*/

/// Sentinel return string for when the user is still editing the line
COMLIN_API extern char* comlinEditMore;

/** Start a non-blocking command line read.
 *
 * This will:
 *
 * 1. Initialize the comlin state passed by the user.
 * 2. Put the terminal in RAW mode.
 * 3. Show the prompt.
 * 4. Return control to the user, that will have to call #comlinEditFeed
 *    each time there is some data arriving in the standard input.
 *
 * The user can also call #comlinHide and #comlinShow if it is required
 * to show some input arriving asynchronously, without mixing it with the
 * currently edited line.
 *
 * When #comlinEditFeed returns non-NULL, the user finished with the line
 * editing session (pressed enter CTRL-D/C): in this case the caller needs to
 * call #comlinEditStop to put back the terminal in normal mode.  This won't
 * destroy the buffer, as long as the #ComlinState is still valid in the
 * context of the caller.
 *
 * @return 0 on success, or -1 if writing to standard output fails.
 */
COMLIN_API int
comlinEditStart(ComlinState* l);

/** This function is part of the multiplexed API of comlin, see the top
 * comment on #comlinEditStart for more information.  Call this function
 * each time there is some data to read from the standard input file
 * descriptor.  In the case of blocking operations, this function can just be
 * called in a loop, and block.
 *
 * The function returns #comlinEditMore to signal that line editing is still
 * in progress, that is, the user didn't yet pressed enter / CTRL-D.  Otherwise
 * the function returns the pointer to the heap-allocated buffer with the
 * edited line, that the user should free with #comlinFreeCommand.
 *
 * On special conditions, NULL is returned and errno is set to `EAGAIN` if the
 * user pressed Ctrl-C, `ENOENT` if the user pressed Ctrl-D, or some other
 * error number on an I/O error.
 */
COMLIN_API char*
comlinEditFeed(ComlinState* l);

/** Finish editing a line.
 *
 * This should be called when #comlinEditFeed returns something a finished
 * command string.  At that point, the user input is in the buffer, and we can
 * restore the terminal to normal mode.
 */
COMLIN_API void
comlinEditStop(ComlinState* l);

/** Return the current command line text.
 *
 * This is used to get the finished command after it has been entered (indicated
 * by #comlinEditFeed returning 0).
 */
COMLIN_API const char*
comlinText(ComlinState* l);

/// Hide the current line, when using the multiplexing API
COMLIN_API void
comlinHide(ComlinState* l);

/// Show the current line, when using the multiplexing API
COMLIN_API void
comlinShow(ComlinState* l);

/**
   @}
   @defgroup comlin_blocking Blocking API
   @{
*/

/** Simple high-level entry point.
 *
 * This checks if the terminal has basic capabilities, and later either calls
 * the line editing function or uses dummy fgets() so that you will be able to
 * type something even in the most desperate of the conditions.
 *
 * @return A newly allocated command string that must be freed with
 * #comlinFreeCommand.
 */
COMLIN_API char*
comlinReadLine(ComlinState* state, const char* prompt);

/// Free a command string returned by comlin()
COMLIN_API void
comlinFreeCommand(void* ptr);

/**
   @}
   @defgroup comlin_completion Completion
   @{
*/

/** A sequence of applicable completions.
 *
 * This is passed to the completion callback, which can add completions to it
 * with #comlinAddCompletion.
 */
typedef struct {
    size_t len;  ///< Number of elements in cvec
    char** cvec; ///< Array of string pointers
} ComlinCompletions;

/// Completion callback
typedef void(ComlinCompletionCallback)(const char*, ComlinCompletions*);

/// Register a callback function to be called for tab-completion
COMLIN_API void
comlinSetCompletionCallback(ComlinState* state, ComlinCompletionCallback* fn);

/** Add completion options for the current input string.
 *
 * This is used by completion callback to add completion options given the
 * input string when the user pressed `TAB`.
 */
COMLIN_API void
comlinAddCompletion(ComlinCompletions* lc, const char* str);

/**
   @}
   @defgroup comlin_history History
   @{
*/

/** Add a new entry to the history.
 *
 * The new entry will be added to the history in memory, which can later be
 * saved explicitly with #comlinHistorySave.
 */
COMLIN_API int
comlinHistoryAdd(ComlinState* state, const char* line);

/** Set the maximum length for the history.
 *
 * This function can be called even if there is already some history, it will
 * make sure to retain just the latest 'len' elements if the new history length
 * length is less than the number of items already in the history.
 */
COMLIN_API int
comlinHistorySetMaxLen(ComlinState* state, int len);

/** Save the history in the specified file.
 *
 * @return 0 on success, otherwise -1.
 */
COMLIN_API int
comlinHistorySave(const ComlinState* state, const char* filename);

/** Load the history from the specified file.
 *
 * @return 0 on success, otherwise -1.
 */
COMLIN_API int
comlinHistoryLoad(ComlinState* state, const char* filename);

/**
   @}
   @defgroup comlin_utilities Utilities
   @{
*/

/// Clear the screen
COMLIN_API void
comlinClearScreen(ComlinState* state);

/// Set whether to use multi-line mode
COMLIN_API void
comlinSetMultiLine(ComlinState* state, bool ml);

/** Enable "mask mode".
 *
 * When this is enabled, the terminal will hide user input with asterisks.
 * This is useful for passwords and other secrets that shouldn't be *
 * displayed.
 */
COMLIN_API void
comlinMaskModeEnable(ComlinState* state);

/** Disable "mask mode".
 *
 * This will return to showing the user input after it was hidden with
 * comlinMaskModeEnable().
 */
COMLIN_API void
comlinMaskModeDisable(ComlinState* state);

/**
   @}
   @}
*/

#ifdef __cplusplus
} // extern "C"
#endif

#endif // COMLIN_COMLIN_H
