param(
    [string]$Cxx = "c++",
    [switch]$Cleanup
)

$localSetup = Join-Path $PSScriptRoot "setup_official_compare.py"

if (Test-Path $localSetup) {
    $root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    python "$root\tests\scripts\setup_official_compare.py" --cxx $Cxx --download-vectors both
    exit $LASTEXITCODE
}

$workspaceRoot = Join-Path (Get-Location) "opuscpp-report"
$zipPath = Join-Path $workspaceRoot "opuscpp-main.zip"
$extractRoot = Join-Path $workspaceRoot "extract"
$repoRoot = Join-Path $extractRoot "opuscpp-main"

New-Item -ItemType Directory -Path $workspaceRoot -Force | Out-Null

try {
    Write-Host "Using workspace: $workspaceRoot"
    Write-Host "Downloading opuscpp full-report bundle..."
    Invoke-WebRequest -Uri "https://codeload.github.com/reg31/opuscpp/zip/refs/heads/main" -OutFile $zipPath
    Expand-Archive -Path $zipPath -DestinationPath $extractRoot -Force
    python "$repoRoot\tests\scripts\setup_official_compare.py" --cxx $Cxx --download-vectors both
    if ($LASTEXITCODE -eq 0) {
        Write-Host "Report artifacts kept in: $workspaceRoot"
    }
    exit $LASTEXITCODE
}
finally {
    if ($Cleanup -and (Test-Path $workspaceRoot)) {
        Remove-Item -Recurse -Force $workspaceRoot
    }
}
