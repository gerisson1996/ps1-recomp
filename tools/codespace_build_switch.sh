#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

KERNEL="${1:-KERNEL.BIN}"
if [[ ! -f "$KERNEL" ]]; then
  echo "KERNEL.BIN não encontrado."
  echo
  echo "Envie o arquivo KERNEL.BIN para a raiz do Codespace"
  echo "ou rode:"
  echo "  ./tools/codespace_build_switch.sh caminho/para/KERNEL.BIN"
  exit 2
fi

echo "== ps1Recomp Switch: build real game =="
echo "Kernel: $KERNEL"
echo

python3 tools/build_switch_real.py "$KERNEL" --jobs 2

cp -f switch/ps1recomp_game.nro ./ps1recomp_game.nro

echo
echo "========================================"
echo "PRONTO"
echo "Arquivo para baixar:"
echo "  ps1recomp_game.nro"
echo
echo "No SD do Switch mantenha:"
echo "  /switch/ps1recomp/KERNEL.BIN"
echo
echo "Opcional para leituras de CD:"
echo "  /switch/ps1recomp/DISC.BIN"
echo "========================================"
