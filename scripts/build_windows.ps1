#Requires -Version 5.1
<#
.SYNOPSIS
    Configures and builds the native microscope module for Windows.

.DESCRIPTION
    Produces release\_microscope_app_cpp.pyd plus the native DLLs it needs.
    Run scripts/build_deps_windows.ps1 first (Qt SerialPort, QScintilla, libtiff).

.EXAMPLE
    powershell -File scripts\build_windows.ps1 -Clean
#>

[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [ValidateSet('Release', 'Debug')] [string]$Config = 'Release',
    [switch]$WithYolo,
    [string]$TensorRtDir = '',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
if (-not $ProjectRoot) { $ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath) }
. (Join-Path $PSScriptRoot 'win_env.ps1')
Enter-MicroscopeEnvironment -ProjectRoot $ProjectRoot
$ErrorActionPreference = 'Continue'

$ProjectRoot = (Resolve-Path $ProjectRoot).Path
$buildDir = Join-Path $ProjectRoot 'build'
$prefix = Join-Path $ProjectRoot 'third_party\x64'
$venvPython = $env:MICROSCOPE_VENV_PYTHON

if (-not (Test-Path (Join-Path $prefix 'include\Qsci\qsciscintilla.h'))) {
    throw 'Third-party dependencies missing. Run scripts/build_deps_windows.ps1 first.'
}

$pybind11Cmake = (& $venvPython -m pybind11 --cmakedir).Trim()
if ($LASTEXITCODE -ne 0 -or -not $pybind11Cmake) {
    throw 'pybind11 is not installed in the project virtual environment.'
}

if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }

$cmakePrefixPath = "$prefix;$pybind11Cmake;$env:QT_PREFIX_PATH"

$yoloArgs = @()
if ($WithYolo -and $TensorRtDir) { $yoloArgs += "-DMICROSCOPE_TENSORRT_DIR=$($TensorRtDir -replace '\\','/')" }

& cmake -S $ProjectRoot -B $buildDir -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DCMAKE_PREFIX_PATH=$cmakePrefixPath" `
    "-DPython_EXECUTABLE=$($venvPython -replace '\\','/')" `
    "-DMICROSCOPE_ENABLE_YOLO=$([bool]$WithYolo)" `
    @yoloArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }

& cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

$module = Join-Path $ProjectRoot 'release\_microscope_app_cpp.pyd'
if (-not (Test-Path $module)) { throw "Expected module not found: $module" }

Write-Host "`nBuilt: $module" -ForegroundColor Green
Get-ChildItem (Split-Path $module) -Include *.pyd, *.dll -Name -File | ForEach-Object { "  $_" }
Write-Host "`nRun with:  .venv\Scripts\python.exe run_microscope_app.py" -ForegroundColor Green
