#!/usr/bin/env pwsh

$ErrorActionPreference = "Stop"

function Get-CMakeCommand {
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmake) {
        return @($cmake.Source)
    }

    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python) {
        try {
            & $python.Source -m cmake --version | Out-Null
            return @($python.Source, "-m", "cmake")
        } catch {
        }
    }

    throw "CMake was not found. Install CMake or run 'python -m pip install cmake'."
}

function Invoke-External {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Command,
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    if ($Command.Count -gt 1) {
        & $Command[0] @($Command[1..($Command.Count - 1)] + $Arguments)
    } else {
        & $Command[0] @Arguments
    }

    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $($Command -join ' ') $($Arguments -join ' ')"
    }
}

function Resolve-VcpkgToolchain {
    $candidates = @(
        (Join-Path $PSScriptRoot "vcpkg\scripts\buildsystems\vcpkg.cmake"),
        (Join-Path $PSScriptRoot "vcpkg\vcpkg\scripts\buildsystems\vcpkg.cmake")
    )

    if ($env:VCPKG_ROOT) {
        $candidates += (Join-Path $env:VCPKG_ROOT "scripts\buildsystems\vcpkg.cmake")
    }

    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    return $null
}

$cmakeCommand = Get-CMakeCommand
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$generatorArgs = @()

if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requiresAny -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath
    if ($vsPath) {
        $generatorArgs = @("-G", "Visual Studio 17 2022", "-A", "x64")
        Write-Host "Using Visual Studio generator from $vsPath" -ForegroundColor Green
    }
}

if (-not (Test-Path "config.json")) {
    Copy-Item "config.example.json" "config.json"
    Write-Host "Created config.json from config.example.json" -ForegroundColor Green
}

if (-not (Test-Path "build")) {
    New-Item -ItemType Directory -Path "build" | Out-Null
}

$configureArgs = @("-B", "build", "-S", ".", "-DCMAKE_BUILD_TYPE=Release")
if ($generatorArgs.Count -gt 0) {
    $configureArgs += $generatorArgs
}

$vcpkgToolchain = Resolve-VcpkgToolchain
if ($vcpkgToolchain) {
    $configureArgs += @("-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain", "-DVCPKG_TARGET_TRIPLET=x64-windows")
    Write-Host "Using vcpkg toolchain: $vcpkgToolchain" -ForegroundColor Green
} else {
    Write-Host "No valid vcpkg toolchain found. Falling back to CMake FetchContent dependencies." -ForegroundColor Yellow
}

Write-Host "[1/3] Configuring project..." -ForegroundColor Cyan
Invoke-External -Command $cmakeCommand -Arguments $configureArgs

Write-Host "[2/3] Building project..." -ForegroundColor Cyan
Invoke-External -Command $cmakeCommand -Arguments @("--build", "build", "--config", "Release", "--parallel", "4")

$testExecutable = Join-Path $PSScriptRoot "build\Release\kabu_micro_edge_tests.exe"
if (-not (Test-Path $testExecutable)) {
    $testExecutable = Join-Path $PSScriptRoot "build\kabu_micro_edge_tests.exe"
}

Write-Host "[3/3] Running tests..." -ForegroundColor Cyan
if (Test-Path $testExecutable) {
    & $testExecutable
    if ($LASTEXITCODE -ne 0) {
        throw "Tests failed with exit code $LASTEXITCODE"
    }
} else {
    Write-Host "Test executable not found: $testExecutable" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Build complete." -ForegroundColor Green
Write-Host "Run the app with:" -ForegroundColor Cyan
Write-Host "  .\build\Release\kabu_micro_edge.exe --config config.json"
Write-Host "Or for a bounded local dry-run:"
Write-Host "  .\build\Release\kabu_micro_edge.exe --config config.json --run-seconds 5"
