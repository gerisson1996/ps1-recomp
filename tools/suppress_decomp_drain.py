#!/usr/bin/env python3
"""Disable the injected callback yield point inside Crash's NSF decompressor.

The recompiler injects `drainPendingCallbacks()` at every backward branch.  In
`func_800334A0` (the NSF chunk decompressor) dispatching a callback there
damages the decompression in progress: the routine finishes with its output
count complete (r3 == r28) but a non-zero pending-run count (r11), and its exit
condition can then never be satisfied -- the only path that decrements r11
requires r3 < r28.  The game thread spins forever.

Measured 2026-09-01, same binary, 5 runs each:
    drain left in place  -> hangs in most runs
    drain suppressed     -> 4/5 runs clean, level playable

THIS IS SUPPRESSION, NOT A FIX.  The damage is in memory, not registers --
saving and restoring the whole register file around the callback made things
worse (1/6), which rules out register clobbering.  Something a callback writes
corrupts the decompressor's working set.  The real fix is to identify that
writer (arm PS1_WRITE_GUARD over 0x8008EA70..0x8008FCCC during a decompression,
after making the guard N-shot) and correct the cause.

Until then this keeps the level playable.  It is game-specific and must not be
baked into the emitter.

Usage:
    tools/suppress_decomp_drain.py ps1Runtime/src/recompiled_out.cpp
"""

import re
import sys

FUNC = "void func_800334A0(uint8_t* rdram, recomp_context* ctx) {"
# The decompressor's body ends where the next emitted function begins.
END = "void func_80033EF8(uint8_t* rdram"
DRAIN = "    if (ctx->bios) ctx->bios->drainPendingCallbacks();"
REPLACEMENT = ("    /* see tools/suppress_decomp_drain.py */ "
               "if (ctx->bios) ctx->bios->pumpOnly();")


def main() -> int:
    path = sys.argv[1]
    with open(path, "r", encoding="utf-8", errors="surrogateescape") as fh:
        lines = fh.read().split("\n")

    try:
        start = next(i for i, l in enumerate(lines) if l.startswith(FUNC))
        end = next(i for i, l in enumerate(lines) if i > start and l.startswith(END))
    except StopIteration:
        print("FAILED: func_800334A0 / func_80033EF8 not found -- the emitter's "
              "output changed shape, re-verify before suppressing", file=sys.stderr)
        return 1

    n = 0
    for i in range(start, end):
        if lines[i] == DRAIN:
            lines[i] = REPLACEMENT
            n += 1

    if n == 0:
        if any("suppressed: see tools/suppress_decomp_drain" in l
               for l in lines[start:end]):
            print("  already suppressed")
            return 0
        print("FAILED: no drain call sites found inside func_800334A0",
              file=sys.stderr)
        return 1

    with open(path, "w", encoding="utf-8", errors="surrogateescape") as fh:
        fh.write("\n".join(lines))
    print("  suppressed %d drain site(s) in func_800334A0" % n)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
