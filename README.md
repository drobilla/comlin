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

Requirements
------------

Comlin requires a reasonably modern POSIX system with a C99 or newer compiler.
It works on terminals that support the following characters and VT-100 escape
sequences:

* ASCII `CR` (Carriage Return): `0x0D`
  * Move the cursor to the start of the current line.
* `EL` (Erase Line): `ESC [ n K`
  * If `n` is 0 or missing, clear from cursor to end of line.
  * If `n` is 1, clear from beginning of line to cursor.
  * If `n` is 2, clear entire line.
* `CUF` (CUrsor Forward): `ESC [ n C`
  * Move the cursor forward `n` characters.
* `CUB` (CUrsor Backward): `ESC [ n D`
  * Move the cursor backward `n` characters.

Basic functionality requires only these, but others may be used conditionally.
If getting the terminal width via the `TIOCGWINSZ` ioctl fails, it will be
requested from the terminal:

* `DSR` (Device Status Report): `ESC [ 6 n`
  * Report the current cursor row `n` and column `m` as `ESC [ n ; m R`.

If that method fails as well, the terminal is assumed to be 80 columns wide.
If multi-line mode is enabled, the cursor may be moved vertically:

* `CUU` (Cursor Up): `ESC [ n A`
  * Move the cursor up `n` lines.
* `CUD` (Cursor Down): Sequence: `ESC [ n B`
  * Move the cursor down `n` lines.

If the screen is cleared, the terminal is asked to return the cursor to home
and erase the display:

* `CUP` (Cursor position): `ESC [ H`
  * Move the cursor to upper left corner.
* `ED` (Erase display): `ESC [ 2 J`
  * Clear the entire screen.

Related projects
----------------

* Comlin is based on [Linenoise](https://github.com/antirez/linenoise)

All credit for the original implementation goes to the authors of linenoise.
The history of this git repository preserves this lineage, but note that comlin
is a separate project with a completely incompatible API.
