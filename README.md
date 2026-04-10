Comlin
======

A minimal C library that implements a command line with history for VT-100
compatible terminals.

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

The implementation is a BSD-licensed C99 library with about a thousand lines of
code in a single file, alongside a header that declares the public API.

History
-------

Comlin is a fork of [Linenoise](https://github.com/antirez/linenoise), mostly
written by Salvatore Sanfilippo.  This git repository preserves that lineage,
but note that comlin is a separate project with a completely incompatible API.
It is functionally similar, but intended to be suitable for packaging.
