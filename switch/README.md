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
