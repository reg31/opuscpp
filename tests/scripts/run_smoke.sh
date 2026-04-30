#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CXX_BIN="${1:-c++}"

if [ -f "$SCRIPT_DIR/../run_smoke.py" ]; then
    ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
    python3 "$ROOT_DIR/tests/run_smoke.py" --cxx "$CXX_BIN"
    exit 0
fi

WORK_DIR="$PWD/opuscpp-smoke"
cleanup() {
    rm -rf "$WORK_DIR"
}

mkdir -p "$WORK_DIR"
echo "Using workspace: $WORK_DIR"
echo "Downloading opuscpp smoke test bundle..."
python3 - "$WORK_DIR" "$CXX_BIN" <<'PY'
import pathlib
import sys
import urllib.request
import zipfile
import subprocess

work_dir = pathlib.Path(sys.argv[1])
cxx = sys.argv[2]
zip_path = work_dir / "opuscpp-main.zip"
extract_root = work_dir / "extract"
repo_root = extract_root / "opuscpp-main"

urllib.request.urlretrieve(
    "https://codeload.github.com/reg31/opuscpp/zip/refs/heads/main",
    zip_path,
)

with zipfile.ZipFile(zip_path) as zf:
    zf.extractall(extract_root)

subprocess.run(
    [sys.executable, str(repo_root / "tests" / "run_smoke.py"), "--cxx", cxx],
    check=True,
)
PY
echo "Artifacts kept in: $WORK_DIR"
