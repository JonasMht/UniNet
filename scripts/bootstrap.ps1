# UniNet: one-command bootstrap for Windows (PowerShell).
#
# Installs vcpkg (if absent), uses it for zlib + lz4, then configures, builds,
# and tests UniNet with Visual Studio's CMake. Safe to re-run.
#
#   .\scripts\bootstrap.ps1                # C++ lib + tests + C ABI
#   .\scripts\bootstrap.ps1 -BuildPython   # also build the Python extension
#
# Prereqs already on the machine: Git, CMake, and the "Desktop development with
# C++" workload in Visual Studio 2022 (or the Build Tools).
param([switch]$BuildPython)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path "$PSScriptRoot/.."
Set-Location $Root

function Say($m){ Write-Host "▶ $m" -ForegroundColor Cyan }
function Ok($m){ Write-Host "✓ $m" -ForegroundColor Green }

# ── 1. vcpkg + dependencies ──────────────────────────────────────────────────
Say "Setting up vcpkg (zlib + lz4)"
if (-not $env:VCPKG_ROOT -or -not (Test-Path $env:VCPKG_ROOT)) {
    $env:VCPKG_ROOT = Join-Path $Root "build-cache/vcpkg"
}
if (-not (Test-Path "$env:VCPKG_ROOT/vcpkg.exe")) {
    Say "Cloning vcpkg into $env:VCPKG_ROOT"
    git clone -q https://github.com/microsoft/vcpkg.git "$env:VCPKG_ROOT"
    & "$env:VCPKG_ROOT/bootstrap-vcpkg.bat" -disableMetrics | Out-Null
}
& "$env:VCPKG_ROOT/vcpkg.exe" install zlib:x64-windows lz4:x64-windows zeromq:x64-windows czmq:x64-windows zyre:x64-windows | Out-Null
$Toolchain = "$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
Ok "vcpkg ready"

# ── 2. Python extension? ─────────────────────────────────────────────────────
$extra = @("-DUNINET_BUILD_CABI=ON")
if ($BuildPython) {
    pip install -q "pybind11>=2.12" 2>$null
    if (python -c "import pybind11" 2>$null) {
        $pydir = (python -m pybind11 --cmakedir).Trim()
        $extra += @("-DUNINET_BUILD_PYTHON=ON", "-Dpybind11_DIR=$pydir")
        Say "Python extension will be built"
    } else { Write-Warning "pybind11 not importable: skipping Python extension." }
}

# ── 3. configure + build ────────────────────────────────────────────────────
Say "Configuring (Release)"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_TOOLCHAIN_FILE="$Toolchain" -DCMAKE_BUILD_TYPE=Release @extra

Say "Building"
cmake --build build --config Release -j

# ── 4. test ──────────────────────────────────────────────────────────────────
Say "Running tests"
ctest --test-dir build -C Release --output-on-failure

Ok "Done. C++ lib + C ABI are in build/Release/. Try: .\build\Release\benchmark.exe 4096 300"
