#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CXX_BIN="${1:-c++}"

if [ -f "$SCRIPT_DIR/../run_smoke.py" ]; then
    ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
    python3 "$ROOT_DIR/tests/run_smoke.py" --cxx "$CXX_BIN"
    exit 0
fi

TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/opuscpp-smoke.XXXXXX")
cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT INT TERM

echo "Downloading opuscpp smoke test bundle..."
python3 - "$TMP_DIR" "$CXX_BIN" <<'PY'
import pathlib
import sys
import urllib.request
import zipfile
import subprocess

tmp_dir = pathlib.Path(sys.argv[1])
cxx = sys.argv[2]
zip_path = tmp_dir / "opuscpp-main.zip"
extract_root = tmp_dir / "extract"
repo_root = extract_root / "opuscpp-main"

urllib.request.urlretrieve(
    "https://github.com/reg31/opuscpp/archive/refs/heads/main.zip",
    zip_path,
)

with zipfile.ZipFile(zip_path) as zf:
    zf.extractall(extract_root)

subprocess.run(
    [sys.executable, str(repo_root / "tests" / "run_smoke.py"), "--cxx", cxx],
    check=True,
)
PY
