#!/usr/bin/env python3
"""Run a game and print the structured metric report.

Replaces log parsing as the source of truth.  Every capped log in this project
under-reports; the JSON written by ps1::metrics does not.

Usage:
    tools/run_report.py configs/crash.toml [seconds]
"""
import json
import os
import subprocess
import sys
import tempfile

INTERESTING_PREFIXES = ("gp0.op.", "draw_otag.", "gool.", "hle.libgpu",
                        "vram_dump.", "fill_rect", "cpu_to_vram")


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    config = sys.argv[1]
    seconds = sys.argv[2] if len(sys.argv) > 2 else "15"

    metrics_path = os.path.join(tempfile.gettempdir(), "ps1_run_report.json")
    if os.path.exists(metrics_path):
        os.remove(metrics_path)

    env = dict(os.environ)
    # `offscreen`, not `dummy`: the renderer needs OpenGL and the dummy video
    # driver has none, so the runtime exits before the first frame.
    env.update(
        PS1_METRICS=metrics_path,
        SDL_VIDEODRIVER=env.get("SDL_VIDEODRIVER", "offscreen"),
        SDL_AUDIODRIVER="dummy",
    )
    cmd = [
        "timeout", "--kill-after=5", seconds,
        "./build/ps1Runtime/ps1Runtime", "--config", config,
    ]
    subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, check=False)

    if not os.path.exists(metrics_path):
        print("no metric report written -- did the runtime reach a shutdown "
              "path?", file=sys.stderr)
        return 1

    with open(metrics_path) as fh:
        report = json.load(fh)

    counters = report.get("counters", {})
    state = report.get("state", {})

    print(f"frames: {counters.get('frames', 0)}")
    print("\ncounters:")
    for key in sorted(counters):
        if key == "frames":
            continue
        if key.startswith(INTERESTING_PREFIXES):
            print(f"  {key:<32} {counters[key]}")
    print("\nstate:")
    for key in sorted(state):
        print(f"  {key:<32} {state[key]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
