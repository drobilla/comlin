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
   @defgroup comlin_status Status Codes
   @{
*/

/// Return status code
typedef enum {
    COMLIN_SUCCESS,      ///< Success
    COMLIN_READING,      ///< Reading continues
    COMLIN_END,          ///< End of input reached
    COMLIN_INTERRUPTED,  ///< Operation interrupted
    COMLIN_BAD_READ,     ///< Failed to read from input
    COMLIN_BAD_WRITE,    ///< Failed to write to output
    COMLIN_BAD_TERMINAL, ///< Failed to configure terminal
} ComlinStatus;

/**
   @}
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
comlinNewState(int stdin_fd, int stdout_fd, const char* prompt);

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

/** Start a non-blocking line edit.
 *
 * This will prepare the terminal if necessary, show the prompt, then return.
 * After this returns successfully, the line can be incrementally read by
 * calling #comlinEditFeed and finally #comlinEditStop.
 *
 * When an edit is in progress, new output can be shown by calling #comlinHide,
 * writing the output to the output stream, then calling #comlinShow to
 * redisplay the current line and reclaim the terminal.
 *
 * When #comlinEditFeed returns non-NULL, the user finished with the line
 * editing session (pressed enter CTRL-D/C): in this case the caller needs to
 * call #comlinEditStop to put back the terminal in normal mode.  This won't
 * destroy the buffer, as long as the #ComlinState is still valid in the
 * context of the caller.
 *
 * @return #COMLIN_SUCCESS, or an error if configuring or writing to the
 * terminal fails.
 */
COMLIN_API ComlinStatus
comlinEditStart(ComlinState* l);

/** Read input during a non-blocking line edit.
 *
 * This will read an input character if possible, and update the state
 * accordingly.  The return status indicates how to proceed:
 *
 * #COMLIN_SUCCESS: Line is entered and available via #comlinText.
 * #COMLIN_READING: Reading should continue.
 * #COMLIN_INTERRUPTED: Input interrupted with Ctrl-C.
 * #COMLIN_END: Input ended with Ctrl-D.
 *
 * @return #COMLIN_SUCCESS, #COMLIN_READING, #COMLIN_END, #COMLIN_INTERRUPTED,
 * or an error if communicating with the terminal failed.
 */
COMLIN_API ComlinStatus
comlinEditFeed(ComlinState* l);

/** Finish a non-blocking line edit.
 *
 * This restores the terminal state modified by #comlinEditStart if necessary,
 * and resets the state for another read.  After this, #comlinEditFeed can no
 * longer be called until a new edit is started, but if a line was entered it
 * is still available via #comlinText.
 */
COMLIN_API ComlinStatus
comlinEditStop(ComlinState* l);

/** Return the text of the current line.
 *
 * After a line has been entered, this returns a pointer to the complete line,
 * excluding any trailing newline or carriage return characters.  It can also
 * be used during an edit to access the incomplete line currently being edited
 * for debugging purposes, although in this state the line should never be
 * interpreted as a "sensible" line the user has entered.
 *
 * @return A pointer to a string, or null.
 */
COMLIN_API const char*
comlinText(ComlinState* l);

/** Pause a non-blocking line edit.
 *
 * This clears the pending input from the screen and resets the terminal mode
 * if necessary.  Then, the application can write its own output, but
 * #comlinEditFeed can't be called until the edit is resumed by #comlinShow.
 */
COMLIN_API void
comlinHide(ComlinState* l);

/** Resume a non-blocking line edit.
 *
 * This will prepare the terminal if necessary, and show the prompt and current
 * line with the cursor where it was.  Then, #comlinEditFeed can be called
 * again to read more input.
 */
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
 * @return #COMLIN_SUCCESS, or an error if reading from the terminal fails.
 */
COMLIN_API ComlinStatus
comlinReadLine(ComlinState* state, const char* prompt);

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
comlinHistorySetMaxLen(ComlinState* state, size_t len);

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
