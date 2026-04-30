#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CXX_BIN="${1:-c++}"

if [ -f "$SCRIPT_DIR/setup_official_compare.py" ]; then
    ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
    python3 "$ROOT_DIR/tests/scripts/setup_official_compare.py" --cxx "$CXX_BIN" --download-vectors both
    exit 0
fi

WORK_DIR="$PWD/opuscpp-report"
mkdir -p "$WORK_DIR"
echo "Using workspace: $WORK_DIR"
echo "Downloading opuscpp full-report bundle..."
python3 - "$WORK_DIR" "$CXX_BIN" <<'PY'
import pathlib
import subprocess
import sys
import urllib.request
import zipfile

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
    [sys.executable, str(repo_root / "tests" / "scripts" / "setup_official_compare.py"), "--cxx", cxx, "--download-vectors", "both"],
    check=True,
)
PY
echo "Report artifacts kept in: $WORK_DIR"
