# PS1Recomp Switch bootstrap

This directory is the first Nintendo Switch target for PS1Recomp.

The initial milestone intentionally does **not** link the desktop runtime. It proves that the repository can produce an ARM64/libnx NRO with devkitA64. This separation is necessary because the current desktop host uses SDL2/OpenGL plus POSIX/x86-oriented debugging code.

## Build locally

Requires devkitPro with devkitA64 and libnx.

```sh
cd switch
make
```

Output: `ps1recomp_switch.nro`.

## Roadmap

1. Bootstrap NRO (current).
2. Split platform-independent runtime code from SDL2/OpenGL host code.
3. Add Switch input backend.
4. Add Switch framebuffer/GPU presentation backend.
5. Add Switch audio backend.
6. Link `recompiled_out.cpp` and boot a recompiled PS1 test program.

## Real-game SD diagnostics

Build the real-game NRO with `git pull origin switch-port` and
`sh tools/r.sh`, then replace the NRO on your SD card.

The real-game harness creates `sdmc:/switch/ps1recomp/logs/` automatically:
- `latest.log`: startup messages and the once-per-second diagnostic console
  blocks from the current run (CPU/BIOS, CD, GPU/VRAM, MDEC, waits and queues).
- `previous.log`: the prior run, preserved when starting again.

Each block is flushed, synced and closed before guest execution resumes.
File logging stops at approximately 8 MiB per run. Folder/write failures are
reported on the console and do not prevent the game from running.
This captures the real-game harness diagnostics, not every runtime stdout/stderr
message or a Horizon/Atmosphere crash dump. An abrupt system/storage failure
can still lose the last write.

To report a problem, let the diagnostic screen run for 20–30 seconds, exit with
PLUS+MINUS if responsive, and send `latest.log`. If you already reopened the
app after the failure, send `previous.log` too.
