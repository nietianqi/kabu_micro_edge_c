# Kabu Micro Edge C++ - Quick Start Guide

This project is a high-performance trading bot written in C++ that connects to the Kabu trading API.

## Prerequisites

Before running this project, you need to install the following tools:

### 1. CMake (Build System)
- Download: https://cmake.org/download/
- Windows: Get the `cmake-3.x.x-windows-x86_64.msi` installer
- During installation, choose "Add CMake to system PATH"

### 2. C++ Compiler (MSVC)
Choose ONE of these options:

**Option A: Visual Studio Build Tools (Recommended, ~3GB)**
- Download: https://visualstudio.microsoft.com/downloads/
- Search for "Build Tools for Visual Studio"
- During installation, select:
  - ✓ Desktop development with C++
  - ✓ C++ core features
  - ✓ MSVC compiler

**Option B: Visual Studio Community (Full IDE, ~5GB)**
- Download: https://visualstudio.microsoft.com/downloads/
- Choose "Visual Studio Community"
- During installation, select "Desktop development with C++"

**Option C: Windows Subsystem for Linux (WSL) - Advanced**
```powershell
# In PowerShell as Administrator:
wsl --install
# Then in WSL terminal:
sudo apt update && sudo apt install -y cmake build-essential
```

## Quick Start (After Installing Required Tools)

### 1. Build the Project

In PowerShell, navigate to the project directory and run:

```powershell
cd d:\kabu_micro_edge_c
.\build.ps1
```

The build script will:
- Configure the project using CMake
- Install C++ dependencies via vcpkg
- Compile the project
- Run tests

### 2. Configure the Application

Edit `config.json` with your trading parameters:

```json
{
  "api_password": "YOUR_KABU_API_PASSWORD",
  "base_url": "http://localhost:18080",
  "ws_url": "ws://localhost:18080/kabusapi/websocket",
  "dry_run": true,  // Set to false when you're ready for real trading
  ...
}
```

### 3. Run the Application

After build completes successfully:

```powershell
# Run with default config
.\build\Release\kabu_micro_edge.exe

# Or run with specific config file
.\build\Release\kabu_micro_edge.exe --config config.json
```

### 4. Run Tests

```powershell
.\build\Release\kabu_micro_edge_tests.exe
```

## Project Structure

- `include/` - C++ header files
- `src/` - C++ source files
- `tests/` - Unit tests
- `tools/` - Python utilities for testing
- `fixtures/` - Test data
- `CMakeLists.txt` - Build configuration
- `vcpkg.json` - C++ dependency manifest

## Dependencies

The project uses these C++ libraries (managed by vcpkg):
- **Boost.Asio** - Networking and async I/O
- **Boost.Beast** - HTTP/WebSocket protocol
- **nlohmann/json** - JSON parsing
- **spdlog** - Logging
- **Google Test** - Unit testing framework

## Troubleshooting

### "CMake not found"
- Install CMake from https://cmake.org/download/
- Make sure to add CMake to system PATH during installation
- Restart your terminal/PowerShell after installation

### "MSVC compiler not found"
- Install Visual Studio Build Tools or Community from https://visualstudio.microsoft.com
- Ensure "Desktop development with C++" workload is selected
- After installation, restart your terminal/PowerShell

### Build fails with "Missing dependencies"
- Delete the `build/` directory
- Run `.\vcpkg\vcpkg.exe install` to re-install dependencies
- Then run `.\build.ps1` again

### Connection errors when running
- Ensure your Kabu API server is running on the configured URL
- Check `config.json` has the correct `api_password`
- For testing, set `"dry_run": true` in config

## Python Tools

The `tools/` directory contains Python utilities:
- `export_fixtures.py` - Generate test fixtures

To run Python tools:
```bash
python tools/python_oracle/export_fixtures.py
```

## Next Steps

1. Install required build tools (CMake + MSVC)
2. Run `.\build.ps1` to build the project
3. Configure `config.json` with your settings
4. Run the executable: `.\build\Release\kabu_micro_edge.exe`

Happy trading! 📈
