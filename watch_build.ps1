#!/usr/bin/env pwsh
# Tail the build log and show real-time progress

param(
    [int]$Lines = 20  # Show last 20 lines
)

Write-Host "Build Process Log Viewer" -ForegroundColor Cyan

# Get all relevant logs
$buildLog = "d:\kabu_micro_edge_c\build\CMakeFiles\CMakeOutput.log"
$vslogs = Get-ChildItem "${env:APPDATA}\Microsoft\VisualStudio\*" -Recurse -Filter "*.log" -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Active Processes:" -ForegroundColor Yellow

$processes = @(
    'vs_BuildTools',
    'cmake', 
    'cl',
    'msbuild',
    'conhost'
)

foreach ($proc in $processes) {
    $p = Get-Process $proc -ErrorAction SilentlyContinue
    if ($p) {
        Write-Host "  + $($p.ProcessName) (PID: $($p.Id))" -ForegroundColor Green
    }
}

Write-Host ""
Write-Host "System Resources:" -ForegroundColor Yellow

$cpu = Get-WmiObject Win32_Processor | Select-Object -ExpandProperty LoadPercentage
$mem = Get-WmiObject Win32_OperatingSystem | Select-Object @{Name="MemoryUsage%";Expression={[math]::Round(($_.TotalVisibleMemorySize - $_.FreePhysicalMemory) / $_.TotalVisibleMemorySize * 100, 2)}}

Write-Host "  CPU: $cpu%"
Write-Host "  RAM: $($mem.'MemoryUsage%')%"

Write-Host ""
Write-Host "Try checking these locations for detailed logs:" -ForegroundColor Gray
Write-Host "  1. ${env:APPDATA}\Microsoft\VisualStudio" -ForegroundColor Gray
Write-Host "  2. ${env:TEMP}" -ForegroundColor Gray
Write-Host "  3. d:\kabu_micro_edge_c\build (once CMake phase starts)" -ForegroundColor Gray

Write-Host ""
Write-Host "Waiting for installation to complete..." -ForegroundColor Yellow
Write-Host ""

Start-Sleep -Seconds 10
