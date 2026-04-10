#!/usr/bin/env python3

# Copyright 2022-2023 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: BSD-2-Clause

"""Run a test by reading input from a file and comparing output with another."""

import argparse
import difflib
import os
import shlex
import subprocess
import sys
import tempfile


def run(out_file, wrapper, command):
    """Run the test and compare the output with the expected output."""

    # Run command and capture actual output
    with subprocess.Popen(
        shlex.split(wrapper) + command,
        stdout=subprocess.PIPE,
    ) as proc:
        actual = proc.stdout.read()

    # Read expected output
    with open(out_file, "rb") as out:
        expected = out.read()

    # Compare actual and expected output
    matcher = difflib.SequenceMatcher(None, actual, expected)
    changes = [o for o in matcher.get_opcodes() if o[0] != "equal"]
    if len(changes) == 0:
        return 0

    # Print changes
    sys.stderr.write("error: Actual output differs from expected\n")
    for tag, i_0, i_1, j_0, j_1 in changes:
        if tag == "replace":
            sys.stderr.write(
                f"replace actual[{i_0}:{i_1}] ({actual[i_0:i_1]})"
                f" with expected[{j_0}:{j_1}] ({expected[j_0:j_1]})\n"
            )
        elif tag == "delete":
            sys.stderr.write(
                f"delete  actual[{i_0}:{i_1}] ({actual[i_0:i_1]})\n"
            )
        elif tag == "insert":
            sys.stderr.write(
                f"insert  expected[{j_0},{j_1}] ({expected[j_0:j_1]})"
                f" at actual[{i_0}:{i_1}]\n"
            )

    return 1


def main():
    """Parse command line arguments and run the test."""

    parser = argparse.ArgumentParser(
        usage="%(prog)s [OPTIONS]... OUT_FILE COMMAND...",
        description=__doc__,
    )

    parser.add_argument("--wrapper", default="", help="executable wrapper")
    parser.add_argument("--history", help="expected final history")

    parser.add_argument("out_file", help="expected output file")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="command")

    args = parser.parse_args(sys.argv[1:])
    if args.history is None:
        return run(args.out_file, args.wrapper, args.command)

    with tempfile.TemporaryDirectory() as temp:
        name = os.path.basename(args.command[-1])
        name = name[:name.index(".")]
        actual_path = os.path.join(temp, name + ".hist.txt")
        command = [args.command[0], "--save", actual_path] + args.command[1:]
        if run(args.out_file, args.wrapper, command):
            return 1

        with open(args.history, "r", encoding="ascii") as expected:
            with open(actual_path, "r", encoding="ascii") as actual:
                same = True
                for line in difflib.unified_diff(
                    list(actual),
                    list(expected),
                    fromfile=actual_path,
                    tofile=args.history,
                ):
                    sys.stderr.write(line)
                    same = False

                if not same:
                    sys.stderr.write("error: Bad final history")
                    return 1

                return 0


if __name__ == "__main__":
    sys.exit(main())
