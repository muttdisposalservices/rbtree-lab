# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

This repository is an empty scaffold for a CS370 red-black tree lab (`rbtree-lab`). At present:

- There are no commits (`git log` has no history).
- `Makefile`, `PROMPTLOG.md`, `include/rbtree.h` are all present but empty (0 bytes).
- `src/rbtree,c` and `tests/test_rbtree,c` are also empty, and their filenames use a comma instead of a period (likely a typo — these are not valid C source filenames as they stand and won't be picked up by a standard `.c` build rule).
- `tests/fuzz.c` is present and empty.

There is no build system, no test runner, and no established architecture yet — none of that should be assumed or invented. Once real code, a Makefile, and tests exist, this file should be updated with actual build/lint/test commands and a description of the real module layout (e.g., how `include/rbtree.h` relates to the implementation in `src/`, and how `tests/test_rbtree.c` and `tests/fuzz.c` exercise it).
