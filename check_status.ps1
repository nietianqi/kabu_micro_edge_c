#!/usr/bin/env pwsh
# Simple status check

Write-Host "Build Status Check" -ForegroundColor Cyan
Write-Host ""

$vsProcess = Get-Process "vs_BuildTools" -ErrorAction SilentlyContinue
$cmakeProcess = Get-Process "cmake" -ErrorAction SilentlyContinue
$clProcess = Get-Process "cl" -ErrorAction SilentlyContinue
$msbuildProcess = Get-Process "msbuild" -ErrorAction SilentlyContinue

if ($vsProcess) {
    Write-Host "VS Build Tools is installing..." -ForegroundColor Yellow
}
elseif ($cmakeProcess) {
    Write-Host "CMake is being processed..." -ForegroundColor Yellow
}
elseif ($clProcess -or $msbuildProcess) {
    Write-Host "Project compilation is in progress..." -ForegroundColor Yellow
}
else {
    Write-Host "No build processes detected." -ForegroundColor Gray
}

Write-Host ""

if (Test-Path "d:\kabu_micro_edge_c\build\Release\kabu_micro_edge.exe") {
    Write-Host "SUCCESS: Binary build complete!" -ForegroundColor Green
    Write-Host "Location: d:\kabu_micro_edge_c\build\Release\kabu_micro_edge.exe" -ForegroundColor Green
    Write-Host ""
    Write-Host "To run: .\build\Release\kabu_micro_edge.exe" -ForegroundColor Cyan
} else {
    Write-Host "Build not yet complete. Waiting..." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Check original build output:"
Write-Host "Get-TerminalOutput cc4a2ab3-ff21-4d12-b870-4fdaf1790c51" -ForegroundColor Gray
