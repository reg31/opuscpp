#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
CXX_BIN="${1:-c++}"

python3 "$ROOT_DIR/tests/run_smoke.py" --cxx "$CXX_BIN"
