param(
    [string]$Cxx = "c++",
    [switch]$Cleanup
)

$localSmoke = Join-Path $PSScriptRoot "..\run_smoke.py"

if (Test-Path $localSmoke) {
    $root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    python "$root\tests\run_smoke.py" --cxx $Cxx
    exit $LASTEXITCODE
}

$workspaceRoot = Join-Path (Get-Location) "opuscpp-smoke"
$zipPath = Join-Path $workspaceRoot "opuscpp-main.zip"
$extractRoot = Join-Path $workspaceRoot "extract"
$repoRoot = Join-Path $extractRoot "opuscpp-main"

New-Item -ItemType Directory -Path $workspaceRoot -Force | Out-Null

try {
    Write-Host "Using workspace: $workspaceRoot"
    Write-Host "Downloading opuscpp smoke test bundle..."
    Invoke-WebRequest -Uri "https://codeload.github.com/reg31/opuscpp/zip/refs/heads/main" -OutFile $zipPath
    Expand-Archive -Path $zipPath -DestinationPath $extractRoot -Force
    python "$repoRoot\tests\run_smoke.py" --cxx $Cxx
    if ($LASTEXITCODE -eq 0) {
        Write-Host "Artifacts kept in: $workspaceRoot"
    }
    exit $LASTEXITCODE
}
finally {
    if ($Cleanup -and (Test-Path $workspaceRoot)) {
        Remove-Item -Recurse -Force $workspaceRoot
    }
}
