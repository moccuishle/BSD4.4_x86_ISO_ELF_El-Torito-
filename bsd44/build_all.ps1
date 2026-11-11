# build_all.ps1 - build kernel + ISO in one shot
$ErrorActionPreference = "Stop"
$ws = Split-Path -Parent $MyInvocation.MyCommand.Path

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $ws "build_kernel.ps1")
if ($LASTEXITCODE -ne 0) { throw "kernel build failed" }

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $ws "build_iso.ps1")
if ($LASTEXITCODE -ne 0) { throw "ISO build failed" }

Write-Host ""
Write-Host "Artifacts:"
Get-ChildItem (Join-Path $ws "out") | Select-Object Name, Length
