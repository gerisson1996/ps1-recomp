#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
exec python3 tools/build_switch_real.py build-real/KERNEL.BIN --jobs 1
