# Automated tool installation script for kabu_micro_edge_c
# This installs CMake and uses pre-installed MSVC if available, or falls back to MinGW

$ErrorActionPreference = "Stop"

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Kabu Micro Edge C++ - Tool Installation" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

# Function to download file
function Download-File {
    param(
        [string]$Url,
        [string]$Output
    )
    Write-Host "Downloading from: $Url" -ForegroundColor Gray
    try {
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $Url -OutFile $Output -UseBasicParsing
        Write-Host "Downloaded: $Output" -ForegroundColor Green
    } catch {
        Write-Host "Download failed: $_" -ForegroundColor Red
        return $false
    }
    return $true
}

# Step 1: Install CMake
Write-Host "`n[1/3] Installing CMake..." -ForegroundColor Yellow
$cmakePath = "C:\Program Files\CMake\bin\cmake.exe"
if (Test-Path $cmakePath) {
    Write-Host "CMake already installed" -ForegroundColor Green
} else {
    $cmakeUrl = "https://github.com/Kitware/CMake/releases/download/v3.28.1/cmake-3.28.1-windows-x86_64.msi"
    $cmakeInstaller = "$env:TEMP\cmake-installer.msi"
    
    if (Download-File -Url $cmakeUrl -Output $cmakeInstaller) {
        Write-Host "Installing CMake..." -ForegroundColor Gray
        Start-Process msiexec.exe -ArgumentList "/i `"$cmakeInstaller`" /quiet" -Wait
        Write-Host "CMake installation complete" -ForegroundColor Green
        Remove-Item $cmakeInstaller -Force -ErrorAction SilentlyContinue
    }
}

# Step 2: Check for MSVC compiler
Write-Host "`n[2/3] Checking for C++ compiler..." -ForegroundColor Yellow
$msvcFound = $false
$vswherePath = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"

if (Test-Path $vswherePath) {
    $vsPath = & $vswherePath -latest -products * -requiresAny -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationpath
    if ($vsPath) {
        Write-Host "Visual Studio found at: $vsPath" -ForegroundColor Green
        $msvcFound = $true
    }
}

if (-not $msvcFound) {
    Write-Host "MSVC not found. Downloading Visual Studio Build Tools..." -ForegroundColor Yellow
    $vsUrl = "https://aka.ms/vs/17/release/vs_BuildTools.exe"
    $vsInstaller = "$env:TEMP\vs_BuildTools.exe"
    
    if (Download-File -Url $vsUrl -Output $vsInstaller) {
        Write-Host "Starting VS Build Tools installer." -ForegroundColor Gray
        Write-Host "Please install with 'Desktop development with C++' workload" -ForegroundColor Yellow
        Start-Process $vsInstaller -Wait
        Remove-Item $vsInstaller -Force -ErrorAction SilentlyContinue
    }
}

# Step 3: Setup project
Write-Host "`n[3/3] Setting up project..." -ForegroundColor Yellow

# Create config.json if not exists
if (-not (Test-Path "config.json")) {
    Copy-Item "config.example.json" "config.json"
    Write-Host "Created config.json" -ForegroundColor Green
}

# Create vcpkg-related files
Write-Host "`nRefresh PATH and run: npm install -g cmake" -ForegroundColor Cyan
Write-Host "Then run: .\build.ps1" -ForegroundColor Cyan

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "Installation Complete!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Cyan
