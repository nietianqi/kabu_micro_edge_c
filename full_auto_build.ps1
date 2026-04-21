#!/usr/bin/env pwsh
# Ultimate automated build script for kabu_micro_edge_c
# Installs all required tools and builds the project - NO USER INTERACTION NEEDED!

$ErrorActionPreference = "SilentlyContinue"
$ProgressPreference = "SilentlyContinue"

# ============================================================================
# COLOR AND LOGGING SETUP
# ============================================================================

$Colors = @{
    Header = "Cyan"
    Success = "Green"
    Info = "Gray"
    Warn = "Yellow"
    Error = "Red"
}

function Log-Header($msg) {
    Write-Host "`n" + ("=" * 70) -ForegroundColor $Colors.Header
    Write-Host "  $msg" -ForegroundColor $Colors.Header
    Write-Host ("=" * 70) -ForegroundColor $Colors.Header
}

function Log-Success($msg) { Write-Host "✓ $msg" -ForegroundColor $Colors.Success }
function Log-Info($msg) { Write-Host "  $msg" -ForegroundColor $Colors.Info }
function Log-Warn($msg) { Write-Host "⚠ $msg" -ForegroundColor $Colors.Warn }
function Log-Error($msg) { Write-Host "✗ $msg" -ForegroundColor $Colors.Error }

# ============================================================================
# PHASE 1: INSTALL VISUAL STUDIO BUILD TOOLS
# ============================================================================

Log-Header "PHASE 1: Installing Build Tools"

try {
    # Check if Visual Studio with C++ is already available
    $vsPath = $null
    $isAlreadyInstalled = $false
    
    $possiblePaths = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Community",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Professional",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\Enterprise",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Community",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Professional",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Enterprise"
    )
    
    foreach ($path in $possiblePaths) {
        if (Test-Path "$path\VC\Tools\MSVC") {
            $vsPath = $path
            Log-Success "Visual Studio found: $path"
            
            # Check if it's the 2022 version (preferred)
            if ($path -like "*2022*") {
                Log-Info "Using Visual Studio 2022"
                $isAlreadyInstalled = $true
                break
            }
        }
    }
    
    if (-not $isAlreadyInstalled) {
        Log-Info "Installing Visual Studio Build Tools 2022..."
        
        try {
            $installerUrl = "https://aka.ms/vs/17/release/vs_BuildTools.exe"
            $installerPath = "$env:TEMP\vs_BuildTools.exe"
            
            Log-Info "Downloading Visual Studio Build Tools (this may take 2-5 minutes)..."
            [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
            
            $webClient = New-Object System.Net.WebClient
            $webClient.DownloadFile($installerUrl, $installerPath)
            
            Log-Success "Download complete"
            
            Log-Info "Starting installation (this may take 10-20 minutes)..."
            Log-Info "Installation will run in the background..."
            
            # Install with C++ workload - no interaction needed
            $args = @(
                "--quiet",
                "--wait",
                "--norestart",
                "--add", "Microsoft.VisualStudio.Workload.CoreEditor",
                "--add", "Microsoft.VisualStudio.Workload.NativeDesktop",
                "--add", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                "--add", "Microsoft.VisualStudio.Component.Windows10SDK.19041"
            )
            
            Start-Process -FilePath $installerPath -ArgumentList $args -Wait -NoNewWindow
            
            Log-Success "Visual Studio Build Tools installed"
            
            # Clean up
            Remove-Item $installerPath -Force -ErrorAction SilentlyContinue
            
            # Find the installation
            foreach ($path in $possiblePaths) {
                if (Test-Path "$path\VC\Tools\MSVC") {
                    $vsPath = $path
                    break
                }
            }
            
            if (-not $vsPath) {
                throw "Visual Studio installation not found after install"
            }
            
            Log-Success "Visual Studio ready at: $vsPath"
            
        } catch {
            Log-Error "Failed to install Visual Studio: $_"
            Log-Info "Installation requires internet connection and ~25GB disk space"
            exit 1
        }
    }
    
} catch {
    Log-Error "Phase 1 failed: $_"
    exit 1
}

# ============================================================================
# PHASE 2: INSTALL CMAKE
# ============================================================================

Log-Header "PHASE 2: Installing CMake"

try {
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    
    if ($cmake) {
        Log-Success "CMake is already installed"
        Log-Info "Location: $($cmake.Source)"
    } else {
        Log-Info "Installing CMake..."
        
        $cmakeUrl = "https://github.com/Kitware/CMake/releases/download/v3.29.0/cmake-3.29.0-windows-x86_64.msi"
        $cmakePath = "$env:TEMP\cmake-installer.msi"
        
        Log-Info "Downloading CMake (this may take 1-2 minutes)..."
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        
        $webClient = New-Object System.Net.WebClient
        $webClient.DownloadFile($cmakeUrl, $cmakePath)
        
        Log-Success "Download complete"
        Log-Info "Installing CMake..."
        
        Start-Process msiexec.exe -ArgumentList "/i `"$cmakePath`" /quiet /norestart" -Wait -NoNewWindow
        
        Log-Success "CMake installed"
        
        # Refresh PATH
        $env:PATH = [System.Environment]::GetEnvironmentVariable("PATH", "Machine") + ";" + [System.Environment]::GetEnvironmentVariable("PATH", "User")
        
        # Clean up
        Remove-Item $cmakePath -Force -ErrorAction SilentlyContinue
        
        # Verify
        $cmake = Get-Command cmake -ErrorAction SilentlyContinue
        if ($cmake) {
            Log-Success "CMake verified"
        }
    }
    
} catch {
    Log-Error "Phase 2 failed: $_"
    exit 1
}

# ============================================================================
# PHASE 3: SETUP ENVIRONMENT
# ============================================================================

Log-Header "PHASE 3: Setting up Build Environment"

try {
    # Setup MSVC environment
    if ($vsPath) {
        Log-Info "Configuring Visual Studio environment..."
        
        # Add MSVC to PATH
        $msvcdirs = Get-ChildItem "$vsPath\VC\Tools\MSVC" -ErrorAction SilentlyContinue | Sort-Object Name -Descending
        if ($msvcdirs.Count -gt 0) {
            $msvcVersion = $msvcdirs[0].Name
            $msvcBin = "$vsPath\VC\Tools\MSVC\$msvcVersion\bin\Hostx64\x64"
            
            if (Test-Path $msvcBin) {
                $env:PATH = "$msvcBin;$env:PATH"
                Log-Success "MSVC tools added to PATH"
            }
        }
        
        # Load VS environment
        $vsDevCmd = "$vsPath\Common7\Tools\VsDevCmd.bat"
        if (Test-Path $vsDevCmd) {
            Log-Info "Loading Visual Studio environment..."
            $output = & cmd /c "call `"$vsDevCmd`" -no_logo && where cl.exe"
            if ($LASTEXITCODE -eq 0) {
                Log-Success "MSVC compiler verified"
            }
        }
    }
    
    # Prepare build directory
    if (-not (Test-Path "build")) {
        New-Item -ItemType Directory -Path "build" -Force | Out-Null
    }
    Log-Success "Build directory ready"
    
    # Create config if missing
    if (-not (Test-Path "config.json")) {
        Copy-Item "config.example.json" "config.json" -Force
        Log-Success "Created config.json"
    } else {
        Log-Success "Config file exists"
    }
    
} catch {
    Log-Error "Phase 3 failed: $_"
    exit 1
}

# ============================================================================
# PHASE 4: BUILD PROJECT
# ============================================================================

Log-Header "PHASE 4: Building Project"

try {
    # Check vcpkg
    $vcpkgToolchain = (Resolve-Path ".\vcpkg\scripts\buildsystems\vcpkg.cmake" -ErrorAction SilentlyContinue).Path
    if (-not $vcpkgToolchain) {
        throw "vcpkg toolchain not found"
    }
    Log-Info "Using vcpkg toolchain"
    
    # CMake configuration with performance optimizations
    Log-Info "Configuring CMake with optimized Release build..."
    
    $cmakeCmd = @(
        "-B", "build",
        "-S", ".",
        "-DCMAKE_TOOLCHAIN_FILE=`"$vcpkgToolchain`"",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_CXX_FLAGS_RELEASE=/O2 /arch:AVX2 /fp:fast /GL",
        "-DCMAKE_EXE_LINKER_FLAGS=/LTCG",
        "-DVCPKG_TARGET_TRIPLET=x64-windows",
        "-G", "Visual Studio 17 2022",
        "-A", "x64"
    )
    
    & cmake @cmakeCmd
    if ($LASTEXITCODE -ne 0) {
        # Try with Visual Studio 16 2019 if 17 fails
        Log-Warn "VS 2022 generator failed, trying VS 2019..."
        $cmakeCmd[-2] = "Visual Studio 16 2019"
        & cmake @cmakeCmd
        
        if ($LASTEXITCODE -ne 0) {
            throw "CMake configuration failed"
        }
    }
    
    Log-Success "CMake configuration complete"
    
    # Build
    Log-Info "Building (this may take 15-30 minutes on first build)..."
    & cmake --build build --config Release --parallel 4
    
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed"
    }
    
    Log-Success "Build complete!"
    
} catch {
    Log-Error "Phase 4 failed: $_"
    exit 1
}

# ============================================================================
# PHASE 5: TEST AND VERIFY
# ============================================================================

Log-Header "PHASE 5: Testing Build"

try {
    if (Test-Path ".\build\Release\kabu_micro_edge_tests.exe") {
        Log-Info "Running unit tests..."
        & ".\build\Release\kabu_micro_edge_tests.exe"
        Log-Success "Tests completed"
    } else {
        Log-Warn "Test executable not found (will be available on next build)"
    }
    
    if (Test-Path ".\build\Release\kabu_micro_edge.exe") {
        Log-Success "Main executable built successfully"
    }
    
} catch {
    Log-Error "Phase 5 failed: $_"
}

# ============================================================================
# FINAL SUMMARY
# ============================================================================

Log-Header "✓ BUILD COMPLETE!"

Write-Host "`nYour application is ready!`n" -ForegroundColor Green

Write-Host "Next steps:`n" -ForegroundColor Cyan

Write-Host "1. Edit your configuration:" -ForegroundColor Yellow
Write-Host "   config.json - Update API credentials and trading settings`n" -ForegroundColor Gray

Write-Host "2. Run the application:" -ForegroundColor Yellow
Write-Host "   .\build\Release\kabu_micro_edge.exe`n" -ForegroundColor Gray

Write-Host "3. Or run with custom config:" -ForegroundColor Yellow
Write-Host "   .\build\Release\kabu_micro_edge.exe --config config.json`n" -ForegroundColor Gray

Write-Host "4. Run tests:" -ForegroundColor Yellow
Write-Host "   .\build\Release\kabu_micro_edge_tests.exe`n" -ForegroundColor Gray

Log-Info "Installation complete at: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
