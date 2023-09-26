Comlin
======

A minimal, zero-config, BSD-licensed, C library that implements a command-line
for VT-100 compatible terminals.

The user interface is a minimal subset of readline's default key bindings:

* Movement
  * Ctrl-a: Move to the start of the line.
  * Ctrl-e: Move to the end of the line.
  * Ctrl-f: Move forward a character.
  * Ctrl-b: Move back a character.
* Editing
  * Backspace: Delete the character before the cursor.
  * Ctrl-d: Delete the character under the cursor.
  * Ctrl-t: Transpose the character under the cursor with the previous one.
* Cutting
  * Ctrl-k: Kill forwards to the end of the line.
  * Ctrl-u: Kill backwards to the start of the line.
  * Ctrl-w: Kill backwards to the start of the current word.
* History
  * Ctrl-p: Fetch the previous command in the history.
  * Ctrl-n: Fetch the next command in the history.
* Session
  * Tab: Auto-complete current input.
  * Ctrl-l:	Clear the screen.

The implementation is a clean C99 library which can be installed and linked
against, or easily copied into another project.


Related projects
----------------

* Comlin is based on [Linenoise](https://github.com/antirez/linenoise)
