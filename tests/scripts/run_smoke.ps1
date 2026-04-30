param(
    [string]$Cxx = "c++",
    [switch]$KeepCheckout
)

$localSmoke = Join-Path $PSScriptRoot "..\run_smoke.py"

if (Test-Path $localSmoke) {
    $root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    python "$root\tests\run_smoke.py" --cxx $Cxx
    exit $LASTEXITCODE
}

$tempRoot = Join-Path $env:TEMP ("opuscpp-smoke-" + [guid]::NewGuid().ToString("N"))
$zipPath = Join-Path $tempRoot "opuscpp-main.zip"
$extractRoot = Join-Path $tempRoot "extract"
$repoRoot = Join-Path $extractRoot "opuscpp-main"

New-Item -ItemType Directory -Path $tempRoot | Out-Null

try {
    Write-Host "Downloading opuscpp smoke test bundle..."
    Invoke-WebRequest -Uri "https://github.com/reg31/opuscpp/archive/refs/heads/main.zip" -OutFile $zipPath
    Expand-Archive -Path $zipPath -DestinationPath $extractRoot -Force
    python "$repoRoot\tests\run_smoke.py" --cxx $Cxx
    exit $LASTEXITCODE
}
finally {
    if (-not $KeepCheckout -and (Test-Path $tempRoot)) {
        Remove-Item -Recurse -Force $tempRoot
    }
}
