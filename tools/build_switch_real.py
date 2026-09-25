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
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HOST_BUILD = ROOT / "build-host"
PRIVATE_BUILD = ROOT / "build-real"
SWITCH_DIR = ROOT / "switch"
GENERATED_CPP = SWITCH_DIR / "source" / "real_recompiled.cpp"
SPLIT_PREFIX = "real_recompiled_part"
SPLIT_HEADER = SWITCH_DIR / "source" / "real_recompiled_shared.h"
SPLIT_DISPATCH = SWITCH_DIR / "source" / "real_recompiled_dispatch.cpp"


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

    # Scope all game-specific compatibility changes to the exact user-provided
    # KERNEL diagnosed on hardware. Matching only header geometry could patch
    # a different executable that happens to share the same load layout.
    kernel_sha256 = hashlib.sha256(kernel.read_bytes()).hexdigest()
    if kernel_sha256 != "bd452bbf7934acb8b17c1017eaa5c30e8ef5f1ea980a7e50a2b7add7f11f8d70":
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

    # The native CD command queue polls internal CD_sync at 0x80026FAC.
    # Hardware diagnostics show a type-2 queue entry remaining pending while
    # cooperative CD state is already Complete (sync=2). Force only this exact
    # KERNEL's internal sync helper through the runtime's synchronized HLE.
    cd_sync_patched = False
    for match in list(block_re.finditer(text)):
        block = match.group(0)
        if 'address = "0x80026FAC"' not in block:
            continue
        patched = re.sub(
            r'(?m)^hle\s*=\s*(?:false|true)\s*$',
            'hle = true',
            block,
            count=1,
        )
        patched = re.sub(
            r'(?m)^name\s*=\s*"[^"]+"\s*$',
            'name = "libcd_CD_sync"',
            patched,
            count=1,
        )
        text = text[:match.start()] + patched + text[match.end():]
        applied.append("CD_sync@80026FAC")
        cd_sync_patched = True
        break

    if not cd_sync_patched:
        additions.append(
            """[[hle_functions]]
subsystem = "CD-ROM"
hle = true
stub_type = "recompile"
library = "libcd"
name = "libcd_CD_sync"
address = "0x80026FAC"
"""
        )
        applied.append("CD_sync@80026FAC")

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

    # Fail early if a future analyzer-format change prevents a compatibility
    # patch from landing. Parse TOML structurally: searching raw text by address
    # is unsafe because the same address also appears in [[functions]].
    with config.open("rb") as fp:
        parsed_config = tomllib.load(fp)
    hle_entries = parsed_config.get("hle_functions", [])
    required = (
        ("0x80019238", "libetc_InterruptCallback4"),
        ("0x800255F8", "libetc_VSync"),
        ("0x80026FAC", "libcd_CD_sync"),
        ("0x8003776C", "libgpu_DrawSync"),
    )
    for expected_addr, expected_name in required:
        matches = [
            entry for entry in hle_entries
            if str(entry.get("address", "")).lower() == expected_addr.lower()
        ]
        if not matches:
            raise RuntimeError(
                f"compat HLE missing from config: address={expected_addr}"
            )
        if not any(
            entry.get("name") == expected_name and entry.get("hle") is True
            for entry in matches
        ):
            raise RuntimeError(
                f"compat HLE did not validate for address={expected_addr}: "
                f"expected name={expected_name!r}, hle=true"
            )


def _write_if_changed(path: Path, data: str) -> None:
    if path.is_file() and path.read_text(encoding="utf-8") == data:
        return
    path.write_text(data, encoding="utf-8")


def split_generated_cpp(source: Path, *, target_bytes: int = 384 * 1024) -> list[Path]:
    """Split the huge generated TU into small independently compiled chunks.

    The recompiler emits all guest functions plus dispatch glue into one C++
    file. That is convenient on large hosts but can make cc1plus exceed the
    memory limit of a small Codespace even at -O0. The generated file already
    contains forward declarations for every guest function, so we can turn the
    declaration prefix into a private header and distribute complete function
    definitions across several translation units without changing guest code.
    """
    text = source.read_text(encoding="utf-8")
    first = re.search(
        r"(?m)^void\s+[A-Za-z_][A-Za-z0-9_]*\(uint8_t\* rdram, "
        r"recomp_context\* ctx\)\s*\{",
        text,
    )
    dispatch_pos = text.find("\n// Dispatch Table\n")
    if first is None or dispatch_pos < 0 or dispatch_pos <= first.start():
        raise RuntimeError("cannot split generated C++: expected function/dispatch markers missing")

    prefix = text[:first.start()]
    body = text[first.start():dispatch_pos]
    dispatch = text[dispatch_pos + 1:]

    starts = [
        m.start()
        for m in re.finditer(
            r"(?m)^void\s+[A-Za-z_][A-Za-z0-9_]*\(uint8_t\* rdram, "
            r"recomp_context\* ctx\)\s*\{",
            body,
        )
    ]
    if not starts:
        raise RuntimeError("cannot split generated C++: no function definitions found")
    starts.append(len(body))
    units = [body[starts[i]:starts[i + 1]] for i in range(len(starts) - 1)]

    header_text = "#pragma once\n" + prefix
    _write_if_changed(SPLIT_HEADER, header_text)

    chunks: list[str] = []
    current: list[str] = []
    current_bytes = 0
    for unit in units:
        size = len(unit.encode("utf-8"))
        if current and current_bytes + size > target_bytes:
            chunks.append("".join(current))
            current = []
            current_bytes = 0
        current.append(unit)
        current_bytes += size
    if current:
        chunks.append("".join(current))

    outputs: list[Path] = []
    expected: set[Path] = set()
    for i, chunk in enumerate(chunks):
        out = SWITCH_DIR / "source" / f"{SPLIT_PREFIX}{i:02d}.cpp"
        expected.add(out)
        data = '#include "real_recompiled_shared.h"\n\n' + chunk
        _write_if_changed(out, data)
        outputs.append(out)

    dispatch_data = '#include "real_recompiled_shared.h"\n\n' + dispatch
    _write_if_changed(SPLIT_DISPATCH, dispatch_data)
    outputs.append(SPLIT_DISPATCH)

    for stale in (SWITCH_DIR / "source").glob(f"{SPLIT_PREFIX}*.cpp"):
        if stale not in expected:
            stale.unlink()

    print(
        f"Split generated C++: {len(chunks)} function part(s) + dispatch "
        f"(target {target_bytes // 1024} KiB/part)",
        flush=True,
    )
    return outputs


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
    generated_next = PRIVATE_BUILD / "real_recompiled.next.cpp"
    generated_next_arg = generated_next.relative_to(ROOT).as_posix()

    run([str(analyzer), kernel_arg, config_arg], env=env)
    apply_kernel_compat_overrides(config, local_kernel)
    run([str(recompiler), config_arg, generated_next_arg], env=env)

    if not generated_next.is_file() or generated_next.stat().st_size < 1024:
        raise RuntimeError("recompiler did not generate a valid real_recompiled.cpp")

    generated_text = generated_next.read_text(encoding="utf-8")
    required_stubs = (
        'psyq_dispatch("libetc_InterruptCallback4", ctx);',
        'psyq_dispatch("libetc_VSync", ctx);',
        'psyq_dispatch("libcd_CD_sync", ctx);',
        'psyq_dispatch("libgpu_DrawSync", ctx);',
    )
    missing = [stub for stub in required_stubs if stub not in generated_text]
    if missing:
        raise RuntimeError(
            "generated C++ missing required compatibility HLE stub(s): " + ", ".join(missing)
        )

    # Preserve the existing timestamp when output is byte-identical. Repeated
    # build-helper attempts can then reuse real_recompiled.o after unrelated
    # failures instead of recompiling the giant translation unit every time.
    same = GENERATED_CPP.is_file() and GENERATED_CPP.read_bytes() == generated_next.read_bytes()
    if same:
        generated_next.unlink()
        print(f"Generated C++ unchanged: {GENERATED_CPP}", flush=True)
    else:
        generated_next.replace(GENERATED_CPP)
        print(f"Generated C++ updated: {GENERATED_CPP}", flush=True)

    print(f"Size: {GENERATED_CPP.stat().st_size:,} bytes")
    split_generated_cpp(GENERATED_CPP)

    # Keep the existing object directory on repeated REAL_GAME builds.
    # The generated C++ timestamp makes make rebuild real_recompiled.o, while
    # already-built runtime objects can be reused. This is especially useful in
    # small Codespaces where a killed cc1plus should be retryable without
    # throwing away all successful compilation work.
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
