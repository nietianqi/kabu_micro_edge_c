# Setup script for kabu_micro_edge_c project
# This script will:
# 1. Download and install CMake
# 2. Set up vcpkg
# 3. Create config.json from config.example.json
# 4. Configure and build the project

Write-Host "=== Kabu Micro Edge C++ Setup ===" -ForegroundColor Green

# Step 1: Check and install CMake
Write-Host "`nStep 1: Setting up CMake..." -ForegroundColor Cyan
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Host "CMake not found. Installing cmake..." -ForegroundColor Yellow
    choco install cmake -y --no-progress 2>$null
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        Write-Host "CMake installation via Chocolatey failed. Please visit https://cmake.org/download/" -ForegroundColor Red
        exit 1
    }
}
Write-Host "CMake: $(cmake --version | Select-Object -First 1)" -ForegroundColor Green

# Step 2: Setup vcpkg
Write-Host "`nStep 2: Setting up vcpkg..." -ForegroundColor Cyan
$vcpkgDir = ".\vcpkg"
if (-not (Test-Path "$vcpkgDir\vcpkg.exe")) {
    Write-Host "Downloading vcpkg tool..." -ForegroundColor Yellow
    $latestRelease = Invoke-RestMethod -Uri "https://api.github.com/repos/microsoft/vcpkg-tool/releases/latest"
    $downloadUrl = $latestRelease.assets | Where-Object { $_.name -eq "vcpkg.exe" } | Select-Object -ExpandProperty browser_download_url
    Invoke-WebRequest -Uri $downloadUrl -OutFile "$vcpkgDir\vcpkg.exe" -ErrorAction Stop
    Write-Host "vcpkg downloaded successfully" -ForegroundColor Green
}

# Step 3: Create config.json
Write-Host "`nStep 3: Setting up configuration..." -ForegroundColor Cyan
if (-not (Test-Path "config.json")) {
    Copy-Item "config.example.json" "config.json"
    Write-Host "Created config.json from config.example.json" -ForegroundColor Green
    Write-Host "Note: Edit config.json to set your API credentials" -ForegroundColor Yellow
}

# Step 4: Install dependencies using vcpkg
Write-Host "`nStep 4: Installing C++ dependencies (this may take a few minutes)..." -ForegroundColor Cyan
& "$vcpkgDir\vcpkg.exe" install --triplet x64-windows

# Step 5: Create build directory and configure CMake
Write-Host "`nStep 5: Configuring CMake..." -ForegroundColor Cyan
if (-not (Test-Path "build")) {
    New-Item -ItemType Directory -Name "build" | Out-Null
}
$toolchain = (Resolve-Path "$vcpkgDir\scripts\buildsystems\vcpkg.cmake").Path
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release

# Step 6: Build the project
Write-Host "`nStep 6: Building project..." -ForegroundColor Cyan
cmake --build build --config Release

Write-Host "`n=== Setup Complete ===" -ForegroundColor Green
Write-Host "To run the application:" -ForegroundColor Cyan
Write-Host "  .\build\Release\kabu_micro_edge.exe" -ForegroundColor Yellow
Write-Host "`nTo run tests:" -ForegroundColor Cyan
Write-Host "  .\build\Release\kabu_micro_edge_tests.exe" -ForegroundColor Yellow
