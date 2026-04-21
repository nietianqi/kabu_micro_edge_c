param(
    [double]$RunSeconds = 0
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $repoRoot

$configPath = Join-Path $repoRoot "config.json"
$binaryPath = Join-Path $repoRoot "build\\Release\\kabu_micro_edge.exe"
$logsPath = Join-Path $repoRoot "logs"

if (-not (Test-Path $configPath)) {
    throw "config.json was not found at $configPath"
}

if (-not (Test-Path $logsPath)) {
    New-Item -ItemType Directory -Path $logsPath | Out-Null
}

if (-not (Test-Path $binaryPath)) {
    Write-Host "Binary not found. Building project first..." -ForegroundColor Yellow
    powershell -ExecutionPolicy Bypass -File (Join-Path $repoRoot "build.ps1")
    if (-not (Test-Path $binaryPath)) {
        throw "Build finished but $binaryPath was not created"
    }
}

$apiReachable = Test-NetConnection -ComputerName "localhost" -Port 18080 -InformationLevel Quiet -WarningAction SilentlyContinue
if (-not $apiReachable) {
    Write-Host "Warning: kabu Station API does not look reachable at http://localhost:18080" -ForegroundColor Yellow
    Write-Host "Make sure kabu Station is running and the API is enabled before trading live." -ForegroundColor Yellow
}

Write-Host "Starting kabu_micro_edge in live mode..." -ForegroundColor Green
if ($RunSeconds -gt 0) {
    & $binaryPath --config $configPath --run-seconds $RunSeconds
} else {
    & $binaryPath --config $configPath
}
exit $LASTEXITCODE
