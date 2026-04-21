#!/usr/bin/env pwsh
# Automatic setup and build script for kabu_micro_edge_c
# This script handles everything: tool installation, build, and run

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

# Colors
$Green = "Green"
$Yellow = "Yellow"
$Red = "Red"
$Cyan = "Cyan"

function Write-Header($text) {
    Write-Host "`n╔════════════════════════════════════════════════════════════╗" -ForegroundColor $Cyan
    Write-Host "║  $text" -ForegroundColor $Cyan
    Write-Host "╚════════════════════════════════════════════════════════════╝" -ForegroundColor $Cyan
}

function Write-Success($text) {
    Write-Host "✓ $text" -ForegroundColor $Green
}

function Write-Info($text) {
    Write-Host "ℹ $text" -ForegroundColor $Cyan
}

function Write-Warn($text) {
    Write-Host "⚠ $text" -ForegroundColor $Yellow
}

function Write-Error-Custom($text) {
    Write-Host "✗ $text" -ForegroundColor $Red
}

# Step 1: Check and install CMake
Write-Header "STEP 1: Checking CMake"

try {
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmake) {
        Write-Success "CMake is installed at: $($cmake.Source)"
        $cmakeVersion = & cmake --version | Select-Object -First 1
        Write-Info $cmakeVersion
    } else {
        throw "CMake not found"
    }
} catch {
    Write-Warn "CMake not found. Installing CMake..."
    
    # Download CMake
    $cmakeUrl = "https://github.com/Kitware/CMake/releases/download/v3.28.3/cmake-3.28.3-windows-x86_64.msi"
    $cmakeInstaller = "$env:TEMP\cmake-installer.msi"
    
    Write-Info "Downloading CMake from: $cmakeUrl"
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $cmakeUrl -OutFile $cmakeInstaller -UseBasicParsing -TimeoutSec 300
        Write-Success "CMake downloaded"
    } catch {
        Write-Error-Custom "Failed to download CMake: $_"
        Write-Info "Please download CMake manually from: https://cmake.org/download/"
        exit 1
    }
    
    Write-Info "Installing CMake (this may take 1-2 minutes)..."
    try {
        Start-Process msiexec.exe -ArgumentList "/i `"$cmakeInstaller`" /quiet /norestart" -Wait -NoNewWindow
        Write-Success "CMake installed"
        
        # Refresh PATH
        $env:PATH = [System.Environment]::GetEnvironmentVariable("PATH", "Machine") + ";" + [System.Environment]::GetEnvironmentVariable("PATH", "User")
        
        # Verify installation
        $cmake = Get-Command cmake -ErrorAction SilentlyContinue
        if ($cmake) {
            Write-Success "CMake verified at: $($cmake.Source)"
        }
    } catch {
        Write-Error-Custom "CMake installation failed: $_"
        exit 1
    } finally {
        Remove-Item $cmakeInstaller -Force -ErrorAction SilentlyContinue
    }
}

# Step 2: Check for MSVC compiler
Write-Header "STEP 2: Checking C++ Compiler"

$hasCompiler = $false
$vsPath = $null

# Try to find Visual Studio installation
$vsPaths = @(
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Community",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Professional",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Enterprise",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Community",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Professional",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Enterprise"
)

foreach ($path in $vsPaths) {
    if (Test-Path "$path\VC\Tools\MSVC") {
        $vsPath = $path
        $hasCompiler = $true
        Write-Success "Found Visual Studio at: $vsPath"
        break
    }
}

if (-not $hasCompiler) {
    Write-Error-Custom "MSVC compiler not found in Visual Studio installation"
    Write-Info "Please install Visual Studio Build Tools with C++ workload from:"
    Write-Info "  https://visualstudio.microsoft.com/downloads/"
    Write-Info "`nOr install Visual Studio Community edition"
    exit 1
}

# Step 3: Setup environment and build
Write-Header "STEP 3: Building Project"

# Find MSVC version
$msvcdirs = Get-ChildItem "$vsPath\VC\Tools\MSVC" -ErrorAction SilentlyContinue
if ($msvcdirs) {
    $msvcVersion = $msvcdirs[-1].Name
    $nativePath = "$vsPath\VC\Tools\MSVC\$msvcVersion\bin\Hostx64\x64"
    
    if (Test-Path $nativePath) {
        $env:PATH = "$nativePath;$env:PATH"
        Write-Success "Added MSVC to PATH: $nativePath"
    }
}

# Add VsDevCmd if available
$vsDevCmd = "$vsPath\Common7\Tools\VsDevCmd.bat"
if (Test-Path $vsDevCmd) {
    Write-Info "Loading Visual Studio environment..."
    & cmd /c "call `"$vsDevCmd`" && set" | Where-Object { $_ -match "^PATH=" } | ForEach-Object {
        $env:PATH = $_.Split("=", 2)[1]
    }
}

# Create build directory
Write-Info "Creating build directory..."
if (-not (Test-Path "build")) {
    New-Item -ItemType Directory -Path "build" -Force | Out-Null
}
Write-Success "Build directory ready"

# Ensure config exists
Write-Info "Checking configuration..."
if (-not (Test-Path "config.json")) {
    Copy-Item "config.example.json" "config.json" -Force
    Write-Success "Created config.json from template"
}

# Get vcpkg toolchain path
$vcpkgToolchain = (Resolve-Path ".\vcpkg\scripts\buildsystems\vcpkg.cmake" -ErrorAction SilentlyContinue).Path
if (-not $vcpkgToolchain) {
    Write-Error-Custom "vcpkg toolchain not found"
    exit 1
}
Write-Info "Using vcpkg toolchain: $vcpkgToolchain"

# Configure with CMake
Write-Info "Running CMake configuration..."
$cmakeArgs = @(
    "-B", "build",
    "-S", ".",
    "-DCMAKE_TOOLCHAIN_FILE=`"$vcpkgToolchain`"",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DVCPKG_TARGET_TRIPLET=x64-windows",
    "-G", "Visual Studio 17 2022"
)

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error-Custom "CMake configuration failed"
    exit 1
}
Write-Success "CMake configuration complete"

# Build project
Write-Info "Building project... (this may take 10-30 minutes on first build)"
& cmake --build build --config Release --parallel 4
if ($LASTEXITCODE -ne 0) {
    Write-Error-Custom "Build failed"
    exit 1
}
Write-Success "Build complete"

# Run tests if executable exists
Write-Header "STEP 4: Running Tests"

if (Test-Path ".\build\Release\kabu_micro_edge_tests.exe") {
    Write-Info "Running unit tests..."
    & ".\build\Release\kabu_micro_edge_tests.exe"
    Write-Success "Tests complete"
} else {
    Write-Warn "Test executable not found (may build on next attempt)"
}

# Display completion message
Write-Header "BUILD SUCCESSFUL!"

Write-Host "`nYour application is ready to run!`n" -ForegroundColor $Green
Write-Host "To start the application:" -ForegroundColor $Cyan
Write-Host "  .\build\Release\kabu_micro_edge.exe" -ForegroundColor $Yellow
Write-Host "`nOr with custom config:" -ForegroundColor $Cyan
Write-Host "  .\build\Release\kabu_micro_edge.exe --config config.json" -ForegroundColor $Yellow
Write-Host "`nMake sure to edit config.json with your settings first!" -ForegroundColor $Yellow
