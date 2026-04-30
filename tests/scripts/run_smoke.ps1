param(
    [string]$Cxx = "c++"
)

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
python "$root\tests\run_smoke.py" --cxx $Cxx
