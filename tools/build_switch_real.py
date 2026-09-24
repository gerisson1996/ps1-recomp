#!/usr/bin/env python3
"""
Build a Nintendo Switch NRO from a user-supplied PS-X EXE (for example
KERNEL.BIN) without ever committing the game binary or generated C++.

Usage:
    python3 tools/build_switch_real.py /path/to/KERNEL.BIN

Prerequisites:
    - CMake + a host C++ compiler
    - devkitPro/devkitA64 + libnx
    - make
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HOST_BUILD = ROOT / "build-host"
PRIVATE_BUILD = ROOT / "build-real"
SWITCH_DIR = ROOT / "switch"
GENERATED_CPP = SWITCH_DIR / "source" / "real_recompiled.cpp"


def run(cmd: list[str], *, cwd: Path = ROOT, env: dict[str, str] | None = None) -> None:
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=str(cwd), env=env, check=True)


def find_exe(base: Path) -> Path | None:
    if base.exists():
        return base
    exe = base.with_suffix(".exe")
    if exe.exists():
        return exe
    return None


def apply_kernel_compat_overrides(config: Path, kernel: Path) -> None:
    """Apply narrowly-scoped HLE overrides for the known bring-up KERNEL."""
    header = kernel.read_bytes()[:0x800]
    if len(header) < 0x20:
        return

    pc = int.from_bytes(header[0x10:0x14], "little")
    load_addr = int.from_bytes(header[0x18:0x1C], "little")
    payload_size = int.from_bytes(header[0x1C:0x20], "little")

    if (pc, load_addr, payload_size) != (0x80010B08, 0x80010000, 985088):
        return

    text = config.read_text(encoding="utf-8")
    additions: list[str] = []
    applied: list[str] = []

    # 0x80019238 is a tiny wrapper equivalent to:
    #     InterruptCallback(4, fn)
    # Its short body collides with a one-argument libcd callback signature, so
    # retarget only this exact address to a dedicated runtime HLE.
    block_re = re.compile(
        r"\[\[hle_functions\]\]\n(?:(?!\n\[\[).)*",
        re.MULTILINE | re.DOTALL,
    )
    for match in list(block_re.finditer(text)):
        block = match.group(0)
        if 'address = "0x80019238"' not in block:
            continue

        patched = block
        patched = re.sub(
            r'(?m)^name\s*=\s*"[^"]+"\s*$',
            'name = "libetc_InterruptCallback4"',
            patched,
            count=1,
        )
        patched = re.sub(
            r'(?m)^library\s*=\s*"[^"]+"\s*$',
            'library = "libetc"',
            patched,
            count=1,
        )
        patched = re.sub(
            r'(?m)^subsystem\s*=\s*"[^"]+"\s*$',
            'subsystem = "VSync"',
            patched,
            count=1,
        )
        patched = re.sub(
            r'(?m)^hle\s*=\s*(?:false|true)\s*$',
            'hle = true',
            patched,
            count=1,
        )

        text = text[:match.start()] + patched + text[match.end():]
        applied.append("InterruptCallback4@80019238")
        break

    if 'name = "libetc_VSync"' not in text:
        additions.append(
            """[[hle_functions]]
subsystem = "VSync"
hle = true
stub_type = "recompile"
library = "libetc"
name = "libetc_VSync"
address = "0x800255F8"
"""
        )
        applied.append("VSync")

    if 'name = "libgpu_DrawSync"' not in text:
        additions.append(
            """[[hle_functions]]
subsystem = "Graphics"
hle = true
stub_type = "recompile"
library = "libgpu"
name = "libgpu_DrawSync"
address = "0x8003776C"
"""
        )
        applied.append("DrawSync")

    config.write_text(text, encoding="utf-8")
    if additions:
        with config.open("a", encoding="utf-8") as out:
            out.write("\n")
            out.write("\n".join(additions))

    if applied:
        print("Applied KERNEL compatibility HLEs: " + ", ".join(applied), flush=True)


def build_host_tools(jobs: int) -> tuple[Path, Path]:
    analyzer = find_exe(HOST_BUILD / "ps1Analyzer" / "ps1Analyzer")
    recompiler = find_exe(HOST_BUILD / "ps1Recomp" / "ps1Recomp")
    if analyzer and recompiler:
        return analyzer, recompiler

    run([
        "cmake", "-S", str(ROOT), "-B", str(HOST_BUILD),
        "-DPS1RECOMP_BUILD_TESTS=OFF",
        "-DPS1RECOMP_BUILD_RUNTIME=OFF",
    ])
    run([
        "cmake", "--build", str(HOST_BUILD),
        "--target", "ps1Analyzer", "ps1Recomp",
        "-j", str(jobs),
    ])

    analyzer = find_exe(HOST_BUILD / "ps1Analyzer" / "ps1Analyzer")
    recompiler = find_exe(HOST_BUILD / "ps1Recomp" / "ps1Recomp")
    if not analyzer or not recompiler:
        raise RuntimeError("Host ps1Analyzer/ps1Recomp build did not produce executables")
    return analyzer, recompiler


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("kernel", type=Path, help="PS-X EXE to recompile (for example KERNEL.BIN)")
    ap.add_argument("--jobs", type=int, default=2, help="parallel build jobs (default: 2)")
    args = ap.parse_args()

    kernel = args.kernel.expanduser().resolve()
    if not kernel.is_file():
        print(f"error: file not found: {kernel}", file=sys.stderr)
        return 2

    magic = kernel.read_bytes()[:8]
    if magic != b"PS-X EXE":
        print("error: input is not a PS-X EXE (missing 'PS-X EXE' magic)", file=sys.stderr)
        return 2

    PRIVATE_BUILD.mkdir(parents=True, exist_ok=True)
    local_kernel = PRIVATE_BUILD / "KERNEL.BIN"
    if kernel != local_kernel.resolve():
        shutil.copyfile(kernel, local_kernel)

    analyzer, recompiler = build_host_tools(max(1, args.jobs))

    env = os.environ.copy()
    env["PS1RECOMP_DATA_DIR"] = str(ROOT / "ps1Analyzer" / "data")

    config = PRIVATE_BUILD / "kernel.toml"
    kernel_arg = local_kernel.relative_to(ROOT).as_posix()
    config_arg = config.relative_to(ROOT).as_posix()
    generated_arg = GENERATED_CPP.relative_to(ROOT).as_posix()

    run([str(analyzer), kernel_arg, config_arg], env=env)
    apply_kernel_compat_overrides(config, local_kernel)
    run([str(recompiler), config_arg, generated_arg], env=env)

    if not GENERATED_CPP.is_file() or GENERATED_CPP.stat().st_size < 1024:
        raise RuntimeError("recompiler did not generate switch/source/real_recompiled.cpp")

    print(f"Generated: {GENERATED_CPP}")
    print(f"Size: {GENERATED_CPP.stat().st_size:,} bytes")

    # Do not rely on the Makefile's optional `clean` target here. Some
    # devkitPro/Codespaces make environments enter the recursive build branch
    # directly, where that target is intentionally absent. Removing the build
    # directory is equivalent and also guarantees that switching from the
    # bootstrap objects to REAL_GAME cannot reuse stale .o files.
    shutil.rmtree(SWITCH_DIR / "build", ignore_errors=True)
    for stale in (
        SWITCH_DIR / "ps1recomp_game.nro",
        SWITCH_DIR / "ps1recomp_game.elf",
        SWITCH_DIR / "ps1recomp_game.map",
    ):
        try:
            stale.unlink()
        except FileNotFoundError:
            pass

    run(["make", f"-j{max(1, args.jobs)}", "REAL_GAME=1"], cwd=SWITCH_DIR)

    nro = SWITCH_DIR / "ps1recomp_game.nro"
    if not nro.is_file():
        raise RuntimeError("Switch build finished without ps1recomp_game.nro")

    print()
    print("SUCCESS")
    print(f"NRO: {nro}")
    print()
    print("On the Switch SD card place:")
    print("  sdmc:/switch/ps1recomp/KERNEL.BIN")
    print("  sdmc:/switch/ps1recomp/DISC.BIN   (optional for first boot; needed for CD reads)")
    print("Launch the NRO. PLUS+MINUS exits the real-game harness.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
,
                         'name = "libetc_InterruptCallback4"', patched, count=1)
        patched = re.sub(r'(?m)^library\s*=\s*"[^"]+"\s*    if 'name = "libetc_VSync"' not in text:
        additions.append(
            """[[hle_functions]]
subsystem = "VSync"
hle = true
stub_type = "recompile"
library = "libetc"
name = "libetc_VSync"
address = "0x800255F8"
"""
        )
        applied.append("VSync")

    if 'name = "libgpu_DrawSync"' not in text:
        additions.append(
            """[[hle_functions]]
subsystem = "Graphics"
hle = true
stub_type = "recompile"
library = "libgpu"
name = "libgpu_DrawSync"
address = "0x8003776C"
"""
        )
        applied.append("DrawSync")

    config.write_text(text, encoding="utf-8")
    if additions:
        with config.open("a", encoding="utf-8") as out:
            out.write("\n")
            out.write("\n".join(additions))

    if applied:
        print("Applied KERNEL compatibility HLEs: " + ", ".join(applied), flush=True)


def build_host_tools(jobs: int) -> tuple[Path, Path]:
    analyzer = find_exe(HOST_BUILD / "ps1Analyzer" / "ps1Analyzer")
    recompiler = find_exe(HOST_BUILD / "ps1Recomp" / "ps1Recomp")
    if analyzer and recompiler:
        return analyzer, recompiler

    run([
        "cmake", "-S", str(ROOT), "-B", str(HOST_BUILD),
        "-DPS1RECOMP_BUILD_TESTS=OFF",
        "-DPS1RECOMP_BUILD_RUNTIME=OFF",
    ])
    run([
        "cmake", "--build", str(HOST_BUILD),
        "--target", "ps1Analyzer", "ps1Recomp",
        "-j", str(jobs),
    ])

    analyzer = find_exe(HOST_BUILD / "ps1Analyzer" / "ps1Analyzer")
    recompiler = find_exe(HOST_BUILD / "ps1Recomp" / "ps1Recomp")
    if not analyzer or not recompiler:
        raise RuntimeError("Host ps1Analyzer/ps1Recomp build did not produce executables")
    return analyzer, recompiler


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("kernel", type=Path, help="PS-X EXE to recompile (for example KERNEL.BIN)")
    ap.add_argument("--jobs", type=int, default=2, help="parallel build jobs (default: 2)")
    args = ap.parse_args()

    kernel = args.kernel.expanduser().resolve()
    if not kernel.is_file():
        print(f"error: file not found: {kernel}", file=sys.stderr)
        return 2

    magic = kernel.read_bytes()[:8]
    if magic != b"PS-X EXE":
        print("error: input is not a PS-X EXE (missing 'PS-X EXE' magic)", file=sys.stderr)
        return 2

    PRIVATE_BUILD.mkdir(parents=True, exist_ok=True)
    local_kernel = PRIVATE_BUILD / "KERNEL.BIN"
    if kernel != local_kernel.resolve():
        shutil.copyfile(kernel, local_kernel)

    analyzer, recompiler = build_host_tools(max(1, args.jobs))

    env = os.environ.copy()
    env["PS1RECOMP_DATA_DIR"] = str(ROOT / "ps1Analyzer" / "data")

    config = PRIVATE_BUILD / "kernel.toml"
    kernel_arg = local_kernel.relative_to(ROOT).as_posix()
    config_arg = config.relative_to(ROOT).as_posix()
    generated_arg = GENERATED_CPP.relative_to(ROOT).as_posix()

    run([str(analyzer), kernel_arg, config_arg], env=env)
    apply_kernel_compat_overrides(config, local_kernel)
    run([str(recompiler), config_arg, generated_arg], env=env)

    if not GENERATED_CPP.is_file() or GENERATED_CPP.stat().st_size < 1024:
        raise RuntimeError("recompiler did not generate switch/source/real_recompiled.cpp")

    print(f"Generated: {GENERATED_CPP}")
    print(f"Size: {GENERATED_CPP.stat().st_size:,} bytes")

    # Do not rely on the Makefile's optional `clean` target here. Some
    # devkitPro/Codespaces make environments enter the recursive build branch
    # directly, where that target is intentionally absent. Removing the build
    # directory is equivalent and also guarantees that switching from the
    # bootstrap objects to REAL_GAME cannot reuse stale .o files.
    shutil.rmtree(SWITCH_DIR / "build", ignore_errors=True)
    for stale in (
        SWITCH_DIR / "ps1recomp_game.nro",
        SWITCH_DIR / "ps1recomp_game.elf",
        SWITCH_DIR / "ps1recomp_game.map",
    ):
        try:
            stale.unlink()
        except FileNotFoundError:
            pass

    run(["make", f"-j{max(1, args.jobs)}", "REAL_GAME=1"], cwd=SWITCH_DIR)

    nro = SWITCH_DIR / "ps1recomp_game.nro"
    if not nro.is_file():
        raise RuntimeError("Switch build finished without ps1recomp_game.nro")

    print()
    print("SUCCESS")
    print(f"NRO: {nro}")
    print()
    print("On the Switch SD card place:")
    print("  sdmc:/switch/ps1recomp/KERNEL.BIN")
    print("  sdmc:/switch/ps1recomp/DISC.BIN   (optional for first boot; needed for CD reads)")
    print("Launch the NRO. PLUS+MINUS exits the real-game harness.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
,
                         'library = "libetc"', patched, count=1)
        patched = re.sub(r'(?m)^subsystem\s*=\s*"[^"]+"\s*    if 'name = "libetc_VSync"' not in text:
        additions.append(
            """[[hle_functions]]
subsystem = "VSync"
hle = true
stub_type = "recompile"
library = "libetc"
name = "libetc_VSync"
address = "0x800255F8"
"""
        )
        applied.append("VSync")

    if 'name = "libgpu_DrawSync"' not in text:
        additions.append(
            """[[hle_functions]]
subsystem = "Graphics"
hle = true
stub_type = "recompile"
library = "libgpu"
name = "libgpu_DrawSync"
address = "0x8003776C"
"""
        )
        applied.append("DrawSync")

    config.write_text(text, encoding="utf-8")
    if additions:
        with config.open("a", encoding="utf-8") as out:
            out.write("\n")
            out.write("\n".join(additions))

    if applied:
        print("Applied KERNEL compatibility HLEs: " + ", ".join(applied), flush=True)


def build_host_tools(jobs: int) -> tuple[Path, Path]:
    analyzer = find_exe(HOST_BUILD / "ps1Analyzer" / "ps1Analyzer")
    recompiler = find_exe(HOST_BUILD / "ps1Recomp" / "ps1Recomp")
    if analyzer and recompiler:
        return analyzer, recompiler

    run([
        "cmake", "-S", str(ROOT), "-B", str(HOST_BUILD),
        "-DPS1RECOMP_BUILD_TESTS=OFF",
        "-DPS1RECOMP_BUILD_RUNTIME=OFF",
    ])
    run([
        "cmake", "--build", str(HOST_BUILD),
        "--target", "ps1Analyzer", "ps1Recomp",
        "-j", str(jobs),
    ])

    analyzer = find_exe(HOST_BUILD / "ps1Analyzer" / "ps1Analyzer")
    recompiler = find_exe(HOST_BUILD / "ps1Recomp" / "ps1Recomp")
    if not analyzer or not recompiler:
        raise RuntimeError("Host ps1Analyzer/ps1Recomp build did not produce executables")
    return analyzer, recompiler


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("kernel", type=Path, help="PS-X EXE to recompile (for example KERNEL.BIN)")
    ap.add_argument("--jobs", type=int, default=2, help="parallel build jobs (default: 2)")
    args = ap.parse_args()

    kernel = args.kernel.expanduser().resolve()
    if not kernel.is_file():
        print(f"error: file not found: {kernel}", file=sys.stderr)
        return 2

    magic = kernel.read_bytes()[:8]
    if magic != b"PS-X EXE":
        print("error: input is not a PS-X EXE (missing 'PS-X EXE' magic)", file=sys.stderr)
        return 2

    PRIVATE_BUILD.mkdir(parents=True, exist_ok=True)
    local_kernel = PRIVATE_BUILD / "KERNEL.BIN"
    if kernel != local_kernel.resolve():
        shutil.copyfile(kernel, local_kernel)

    analyzer, recompiler = build_host_tools(max(1, args.jobs))

    env = os.environ.copy()
    env["PS1RECOMP_DATA_DIR"] = str(ROOT / "ps1Analyzer" / "data")

    config = PRIVATE_BUILD / "kernel.toml"
    kernel_arg = local_kernel.relative_to(ROOT).as_posix()
    config_arg = config.relative_to(ROOT).as_posix()
    generated_arg = GENERATED_CPP.relative_to(ROOT).as_posix()

    run([str(analyzer), kernel_arg, config_arg], env=env)
    apply_kernel_compat_overrides(config, local_kernel)
    run([str(recompiler), config_arg, generated_arg], env=env)

    if not GENERATED_CPP.is_file() or GENERATED_CPP.stat().st_size < 1024:
        raise RuntimeError("recompiler did not generate switch/source/real_recompiled.cpp")

    print(f"Generated: {GENERATED_CPP}")
    print(f"Size: {GENERATED_CPP.stat().st_size:,} bytes")

    # Do not rely on the Makefile's optional `clean` target here. Some
    # devkitPro/Codespaces make environments enter the recursive build branch
    # directly, where that target is intentionally absent. Removing the build
    # directory is equivalent and also guarantees that switching from the
    # bootstrap objects to REAL_GAME cannot reuse stale .o files.
    shutil.rmtree(SWITCH_DIR / "build", ignore_errors=True)
    for stale in (
        SWITCH_DIR / "ps1recomp_game.nro",
        SWITCH_DIR / "ps1recomp_game.elf",
        SWITCH_DIR / "ps1recomp_game.map",
    ):
        try:
            stale.unlink()
        except FileNotFoundError:
            pass

    run(["make", f"-j{max(1, args.jobs)}", "REAL_GAME=1"], cwd=SWITCH_DIR)

    nro = SWITCH_DIR / "ps1recomp_game.nro"
    if not nro.is_file():
        raise RuntimeError("Switch build finished without ps1recomp_game.nro")

    print()
    print("SUCCESS")
    print(f"NRO: {nro}")
    print()
    print("On the Switch SD card place:")
    print("  sdmc:/switch/ps1recomp/KERNEL.BIN")
    print("  sdmc:/switch/ps1recomp/DISC.BIN   (optional for first boot; needed for CD reads)")
    print("Launch the NRO. PLUS+MINUS exits the real-game harness.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
,
                         'subsystem = "VSync"', patched, count=1)
        patched = re.sub(r'(?m)^hle\s*=\s*(?:false|true)\s*    if 'name = "libetc_VSync"' not in text:
        additions.append(
            """[[hle_functions]]
subsystem = "VSync"
hle = true
stub_type = "recompile"
library = "libetc"
name = "libetc_VSync"
address = "0x800255F8"
"""
        )
        applied.append("VSync")

    if 'name = "libgpu_DrawSync"' not in text:
        additions.append(
            """[[hle_functions]]
subsystem = "Graphics"
hle = true
stub_type = "recompile"
library = "libgpu"
name = "libgpu_DrawSync"
address = "0x8003776C"
"""
        )
        applied.append("DrawSync")

    config.write_text(text, encoding="utf-8")
    if additions:
        with config.open("a", encoding="utf-8") as out:
            out.write("\n")
            out.write("\n".join(additions))

    if applied:
        print("Applied KERNEL compatibility HLEs: " + ", ".join(applied), flush=True)


def build_host_tools(jobs: int) -> tuple[Path, Path]:
    analyzer = find_exe(HOST_BUILD / "ps1Analyzer" / "ps1Analyzer")
    recompiler = find_exe(HOST_BUILD / "ps1Recomp" / "ps1Recomp")
    if analyzer and recompiler:
        return analyzer, recompiler

    run([
        "cmake", "-S", str(ROOT), "-B", str(HOST_BUILD),
        "-DPS1RECOMP_BUILD_TESTS=OFF",
        "-DPS1RECOMP_BUILD_RUNTIME=OFF",
    ])
    run([
        "cmake", "--build", str(HOST_BUILD),
        "--target", "ps1Analyzer", "ps1Recomp",
        "-j", str(jobs),
    ])

    analyzer = find_exe(HOST_BUILD / "ps1Analyzer" / "ps1Analyzer")
    recompiler = find_exe(HOST_BUILD / "ps1Recomp" / "ps1Recomp")
    if not analyzer or not recompiler:
        raise RuntimeError("Host ps1Analyzer/ps1Recomp build did not produce executables")
    return analyzer, recompiler


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("kernel", type=Path, help="PS-X EXE to recompile (for example KERNEL.BIN)")
    ap.add_argument("--jobs", type=int, default=2, help="parallel build jobs (default: 2)")
    args = ap.parse_args()

    kernel = args.kernel.expanduser().resolve()
    if not kernel.is_file():
        print(f"error: file not found: {kernel}", file=sys.stderr)
        return 2

    magic = kernel.read_bytes()[:8]
    if magic != b"PS-X EXE":
        print("error: input is not a PS-X EXE (missing 'PS-X EXE' magic)", file=sys.stderr)
        return 2

    PRIVATE_BUILD.mkdir(parents=True, exist_ok=True)
    local_kernel = PRIVATE_BUILD / "KERNEL.BIN"
    if kernel != local_kernel.resolve():
        shutil.copyfile(kernel, local_kernel)

    analyzer, recompiler = build_host_tools(max(1, args.jobs))

    env = os.environ.copy()
    env["PS1RECOMP_DATA_DIR"] = str(ROOT / "ps1Analyzer" / "data")

    config = PRIVATE_BUILD / "kernel.toml"
    kernel_arg = local_kernel.relative_to(ROOT).as_posix()
    config_arg = config.relative_to(ROOT).as_posix()
    generated_arg = GENERATED_CPP.relative_to(ROOT).as_posix()

    run([str(analyzer), kernel_arg, config_arg], env=env)
    apply_kernel_compat_overrides(config, local_kernel)
    run([str(recompiler), config_arg, generated_arg], env=env)

    if not GENERATED_CPP.is_file() or GENERATED_CPP.stat().st_size < 1024:
        raise RuntimeError("recompiler did not generate switch/source/real_recompiled.cpp")

    print(f"Generated: {GENERATED_CPP}")
    print(f"Size: {GENERATED_CPP.stat().st_size:,} bytes")

    # Do not rely on the Makefile's optional `clean` target here. Some
    # devkitPro/Codespaces make environments enter the recursive build branch
    # directly, where that target is intentionally absent. Removing the build
    # directory is equivalent and also guarantees that switching from the
    # bootstrap objects to REAL_GAME cannot reuse stale .o files.
    shutil.rmtree(SWITCH_DIR / "build", ignore_errors=True)
    for stale in (
        SWITCH_DIR / "ps1recomp_game.nro",
        SWITCH_DIR / "ps1recomp_game.elf",
        SWITCH_DIR / "ps1recomp_game.map",
    ):
        try:
            stale.unlink()
        except FileNotFoundError:
            pass

    run(["make", f"-j{max(1, args.jobs)}", "REAL_GAME=1"], cwd=SWITCH_DIR)

    nro = SWITCH_DIR / "ps1recomp_game.nro"
    if not nro.is_file():
        raise RuntimeError("Switch build finished without ps1recomp_game.nro")

    print()
    print("SUCCESS")
    print(f"NRO: {nro}")
    print()
    print("On the Switch SD card place:")
    print("  sdmc:/switch/ps1recomp/KERNEL.BIN")
    print("  sdmc:/switch/ps1recomp/DISC.BIN   (optional for first boot; needed for CD reads)")
    print("Launch the NRO. PLUS+MINUS exits the real-game harness.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
,
                         'hle = true', patched, count=1)
        text = text[:match.start()] + patched + text[match.end():]
        applied.append("InterruptCallback4@80019238")
        break

    if 'name = "libetc_VSync"' not in text:
        additions.append(
            """[[hle_functions]]
subsystem = "VSync"
hle = true
stub_type = "recompile"
library = "libetc"
name = "libetc_VSync"
address = "0x800255F8"
"""
        )
        applied.append("VSync")

    if 'name = "libgpu_DrawSync"' not in text:
        additions.append(
            """[[hle_functions]]
subsystem = "Graphics"
hle = true
stub_type = "recompile"
library = "libgpu"
name = "libgpu_DrawSync"
address = "0x8003776C"
"""
        )
        applied.append("DrawSync")

    config.write_text(text, encoding="utf-8")
    if additions:
        with config.open("a", encoding="utf-8") as out:
            out.write("\n")
            out.write("\n".join(additions))

    if applied:
        print("Applied KERNEL compatibility HLEs: " + ", ".join(applied), flush=True)


def build_host_tools(jobs: int) -> tuple[Path, Path]:
    analyzer = find_exe(HOST_BUILD / "ps1Analyzer" / "ps1Analyzer")
    recompiler = find_exe(HOST_BUILD / "ps1Recomp" / "ps1Recomp")
    if analyzer and recompiler:
        return analyzer, recompiler

    run([
        "cmake", "-S", str(ROOT), "-B", str(HOST_BUILD),
        "-DPS1RECOMP_BUILD_TESTS=OFF",
        "-DPS1RECOMP_BUILD_RUNTIME=OFF",
    ])
    run([
        "cmake", "--build", str(HOST_BUILD),
        "--target", "ps1Analyzer", "ps1Recomp",
        "-j", str(jobs),
    ])

    analyzer = find_exe(HOST_BUILD / "ps1Analyzer" / "ps1Analyzer")
    recompiler = find_exe(HOST_BUILD / "ps1Recomp" / "ps1Recomp")
    if not analyzer or not recompiler:
        raise RuntimeError("Host ps1Analyzer/ps1Recomp build did not produce executables")
    return analyzer, recompiler


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("kernel", type=Path, help="PS-X EXE to recompile (for example KERNEL.BIN)")
    ap.add_argument("--jobs", type=int, default=2, help="parallel build jobs (default: 2)")
    args = ap.parse_args()

    kernel = args.kernel.expanduser().resolve()
    if not kernel.is_file():
        print(f"error: file not found: {kernel}", file=sys.stderr)
        return 2

    magic = kernel.read_bytes()[:8]
    if magic != b"PS-X EXE":
        print("error: input is not a PS-X EXE (missing 'PS-X EXE' magic)", file=sys.stderr)
        return 2

    PRIVATE_BUILD.mkdir(parents=True, exist_ok=True)
    local_kernel = PRIVATE_BUILD / "KERNEL.BIN"
    if kernel != local_kernel.resolve():
        shutil.copyfile(kernel, local_kernel)

    analyzer, recompiler = build_host_tools(max(1, args.jobs))

    env = os.environ.copy()
    env["PS1RECOMP_DATA_DIR"] = str(ROOT / "ps1Analyzer" / "data")

    config = PRIVATE_BUILD / "kernel.toml"
    kernel_arg = local_kernel.relative_to(ROOT).as_posix()
    config_arg = config.relative_to(ROOT).as_posix()
    generated_arg = GENERATED_CPP.relative_to(ROOT).as_posix()

    run([str(analyzer), kernel_arg, config_arg], env=env)
    apply_kernel_compat_overrides(config, local_kernel)
    run([str(recompiler), config_arg, generated_arg], env=env)

    if not GENERATED_CPP.is_file() or GENERATED_CPP.stat().st_size < 1024:
        raise RuntimeError("recompiler did not generate switch/source/real_recompiled.cpp")

    print(f"Generated: {GENERATED_CPP}")
    print(f"Size: {GENERATED_CPP.stat().st_size:,} bytes")

    # Do not rely on the Makefile's optional `clean` target here. Some
    # devkitPro/Codespaces make environments enter the recursive build branch
    # directly, where that target is intentionally absent. Removing the build
    # directory is equivalent and also guarantees that switching from the
    # bootstrap objects to REAL_GAME cannot reuse stale .o files.
    shutil.rmtree(SWITCH_DIR / "build", ignore_errors=True)
    for stale in (
        SWITCH_DIR / "ps1recomp_game.nro",
        SWITCH_DIR / "ps1recomp_game.elf",
        SWITCH_DIR / "ps1recomp_game.map",
    ):
        try:
            stale.unlink()
        except FileNotFoundError:
            pass

    run(["make", f"-j{max(1, args.jobs)}", "REAL_GAME=1"], cwd=SWITCH_DIR)

    nro = SWITCH_DIR / "ps1recomp_game.nro"
    if not nro.is_file():
        raise RuntimeError("Switch build finished without ps1recomp_game.nro")

    print()
    print("SUCCESS")
    print(f"NRO: {nro}")
    print()
    print("On the Switch SD card place:")
    print("  sdmc:/switch/ps1recomp/KERNEL.BIN")
    print("  sdmc:/switch/ps1recomp/DISC.BIN   (optional for first boot; needed for CD reads)")
    print("Launch the NRO. PLUS+MINUS exits the real-game harness.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
