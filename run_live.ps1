param(
    [double]$RunSeconds = 0,
    [switch]$HealthCheck
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $repoRoot

$configPath = Join-Path $repoRoot "config.json"
$binaryPath = Join-Path $repoRoot "build\\Release\\kabu_micro_edge.exe"
$logsPath = Join-Path $repoRoot "logs"
$healthLogPath = Join-Path $logsPath "health-check.log"

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
if ($HealthCheck) {
    Write-Host "Running live health check (token/register/ws/first market data)..." -ForegroundColor Cyan
}

$arguments = @("--config", $configPath)
if ($HealthCheck) {
    $arguments += "--health-check"
}
if ($RunSeconds -gt 0) {
    $arguments += @("--run-seconds", $RunSeconds)
}

if ($HealthCheck) {
    $startedAt = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    "=== health-check started at $startedAt ===" | Tee-Object -FilePath $healthLogPath -Append | Out-Host
    $stdoutPath = [System.IO.Path]::GetTempFileName()
    $stderrPath = [System.IO.Path]::GetTempFileName()
    try {
        $process = Start-Process `
            -FilePath $binaryPath `
            -ArgumentList $arguments `
            -NoNewWindow `
            -Wait `
            -PassThru `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath
        $exitCode = $process.ExitCode

        foreach ($path in @($stdoutPath, $stderrPath)) {
            if (Test-Path $path) {
                $content = Get-Content -Path $path -Raw
                if (-not [string]::IsNullOrEmpty($content)) {
                    $content | Tee-Object -FilePath $healthLogPath -Append | Out-Host
                }
            }
        }
    } finally {
        Remove-Item -LiteralPath $stdoutPath, $stderrPath -ErrorAction SilentlyContinue
    }
    $finishedAt = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    "=== health-check finished at $finishedAt exit_code=$exitCode ===" | Tee-Object -FilePath $healthLogPath -Append | Out-Host
    exit $exitCode
}

& $binaryPath @arguments
exit $LASTEXITCODE
