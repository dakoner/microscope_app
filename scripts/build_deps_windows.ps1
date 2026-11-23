#Requires -Version 5.1
<#
.SYNOPSIS
    Builds the third-party C/C++ dependencies the microscope app needs on Windows.

.DESCRIPTION
    Builds and installs, into third_party\x64 (bin / include / lib):
      * libtiff        (BigTIFF export used by MainWindow.cpp)
      * Qt SerialPort  (CNC stage + LED controller; not part of the local Qt kit)
      * QScintilla     (Python script editor widget)
      * CUDA CUDART    (headers + cudart.lib + cudart64_12.dll, from the NVIDIA
                        PyPI wheel; only needed for the YOLO/TensorRT build)

    TensorRT itself is not redistributable through PyPI on Windows, so it is
    unpacked from an official NVIDIA zip instead: pass -TensorRtZip <path> for a
    zip you downloaded from developer.nvidia.com, or -TensorRtUrl <url> for a
    publicly served one.  It lands in third_party\TensorRT-<version>.

    Sources are expected under third_party\ (see DOWNLOADS in this file).  Run
    with -Force to rebuild from scratch.
#>

[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [string]$TensorRtZip = '',
    [string]$TensorRtUrl = '',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
if (-not $ProjectRoot) { $ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath) }
. (Join-Path $PSScriptRoot 'win_env.ps1')
Enter-MicroscopeEnvironment -ProjectRoot $ProjectRoot
# Native build tools write progress/warnings to stderr; rely on $LASTEXITCODE instead.
$ErrorActionPreference = 'Continue'

$ProjectRoot = (Resolve-Path $ProjectRoot).Path
$thirdParty = Join-Path $ProjectRoot 'third_party'
$prefix = Join-Path $thirdParty 'x64'
$buildRoot = Join-Path $thirdParty 'build'
New-Item -ItemType Directory -Force -Path $prefix, $buildRoot | Out-Null

function Invoke-BuildStep {
    param([string]$Name, [string]$Stamp, [scriptblock]$Body)

    if (-not $Force -and (Test-Path $Stamp)) {
        Write-Host "`n=== $Name : already built, skipping (use -Force to rebuild) ===" -ForegroundColor DarkGray
        return
    }
    Write-Host "`n=== $Name ===" -ForegroundColor Cyan
    & $Body
    if ($LASTEXITCODE -ne 0) { throw "$Name failed with exit code $LASTEXITCODE" }
    New-Item -ItemType File -Force -Path $Stamp | Out-Null
}

function Invoke-CMakeBuild {
    param([string]$Source, [string]$Build, [string]$InstallPrefix, [string[]]$ConfigureArgs)

    if ($Force -and (Test-Path $Build)) { Remove-Item -Recurse -Force $Build }
    $cmakePrefix = "$InstallPrefix;$($env:QT_PREFIX_PATH)"
    & cmake -S $Source -B $Build -G Ninja `
        '-DCMAKE_BUILD_TYPE=Release' `
        "-DCMAKE_INSTALL_PREFIX=$InstallPrefix" `
        "-DCMAKE_PREFIX_PATH=$cmakePrefix" `
        @ConfigureArgs
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed for $Source" }
    & cmake --build $Build
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed for $Source" }
    & cmake --install $Build
    if ($LASTEXITCODE -ne 0) { throw "cmake install failed for $Source" }
}

# ---------------------------------------------------------------------------
# libtiff (BigTIFF writer used by MainWindow.cpp)
# ---------------------------------------------------------------------------
$tiffSrc = Join-Path $thirdParty 'tiff-4.7.1'
Invoke-BuildStep 'libtiff 4.7.1' (Join-Path $buildRoot 'libtiff.stamp') {
    Invoke-CMakeBuild -Source $tiffSrc -Build (Join-Path $buildRoot 'tiff') -InstallPrefix $prefix -ConfigureArgs @(
        '-Dtiff-tools=OFF', '-Dtiff-tests=OFF', '-Dzlib=OFF', '-Djpeg=OFF',
        '-Djbig=OFF', '-Dlzma=OFF', '-Dzstd=OFF', '-Dwebp=OFF', '-Dcxx=ON', '-Drpath=OFF'
    )
}

# ---------------------------------------------------------------------------
# Qt SerialPort (built against the local Qt kit so the ABI matches PySide6)
# ---------------------------------------------------------------------------
$serialSrc = Get-ChildItem $thirdParty -Directory |
    Where-Object { $_.Name -like 'qtserialport-everywhere-src-*' } |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $serialSrc) { throw "qtserialport sources not found in $thirdParty" }

Invoke-BuildStep 'Qt SerialPort' (Join-Path $buildRoot 'qtserialport.stamp') {
    Invoke-CMakeBuild -Source $serialSrc -Build (Join-Path $buildRoot 'qtserialport') -InstallPrefix $prefix -ConfigureArgs @(
        "-DCMAKE_PREFIX_PATH=$env:QT_PREFIX_PATH;$prefix"
    )
}

# ---------------------------------------------------------------------------
# QScintilla (CMake wrapper around the upstream qmake sources: nmake has no
# parallel build on Windows, Ninja does)
# ---------------------------------------------------------------------------
$qsciSrc = Get-ChildItem $thirdParty -Directory |
    Where-Object { $_.Name -like 'QScintilla_src-*' } |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $qsciSrc) { throw "QScintilla sources not found in $thirdParty" }

Invoke-BuildStep 'QScintilla' (Join-Path $buildRoot 'qscintilla.stamp') {
    Invoke-CMakeBuild -Source (Join-Path $thirdParty 'qscintilla_cmake') `
        -Build (Join-Path $buildRoot 'qscintilla') -InstallPrefix $prefix -ConfigureArgs @(
            "-DQSCI_SRC_DIR=$qsciSrc",
            "-DCMAKE_PREFIX_PATH=$env:QT_PREFIX_PATH;$prefix"
        )
}

# ---------------------------------------------------------------------------
# CUDA CUDART + CRT headers (cuda_runtime_api.h, cudart.lib, cudart64_13.dll)
#
# The app has no .cu sources, so the multi-gigabyte CUDA toolkit is not needed:
# NVIDIA's public CUDA redistributables carry exactly the headers and libraries
# that YOLOInferenceWorker.cpp uses.  CUDA 13 keeps the crt/ headers that
# cuda_runtime_api.h includes in a separate package from cudart.  The version
# must match the CUDA variant of the TensorRT package - TensorRT 11.3.0
# packages are built against CUDA 13.4.
# ---------------------------------------------------------------------------
$cudaVersion = '13.4.92'
$cudaPackages = @('cuda_cudart', 'cuda_crt')

Invoke-BuildStep 'CUDA 13.4' (Join-Path $buildRoot 'cudart.stamp') {
    $ErrorActionPreference = 'Stop'
    foreach ($package in $cudaPackages) {
        $archive = "${package}-windows-x86_64-$cudaVersion-archive.zip"
        $url = "https://developer.download.nvidia.com/compute/cuda/redist/$package/windows-x86_64/$archive"
        $zip = Join-Path $thirdParty $archive
        if (-not (Test-Path $zip)) {
            Write-Host "Downloading $archive"
            Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
        }
        $unpacked = Join-Path $buildRoot $package
        if (Test-Path $unpacked) { Remove-Item -Recurse -Force $unpacked }
        Expand-Archive -Path $zip -DestinationPath $unpacked -Force

        $root = (Get-ChildItem $unpacked -Directory | Select-Object -First 1).FullName
        foreach ($part in @(
            @{ source = 'include'; destination = (Join-Path $prefix 'include') },
            @{ source = 'lib/x64'; destination = (Join-Path $prefix 'lib') },
            @{ source = 'bin/x64'; destination = (Join-Path $prefix 'bin') })) {
            $from = Join-Path $root $part.source
            if (Test-Path $from) {
                Copy-Item -Path (Join-Path $from '*') -Destination $part.destination -Recurse -Force
            }
        }
    }
    $global:LASTEXITCODE = 0
}

# ---------------------------------------------------------------------------
# TensorRT (YOLO object detection).  NVIDIA does not publish Windows TensorRT
# wheels, so this unpacks the official zip from developer.nvidia.com.
# ---------------------------------------------------------------------------
$trtDir = Get-ChildItem $thirdParty -Directory -Filter 'TensorRT-*' -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $trtDir) {
    if ($TensorRtUrl -and -not $TensorRtZip) {
        $name = Split-Path -Leaf ($TensorRtUrl -split '\?')[0]
        $TensorRtZip = Join-Path $thirdParty $name
        Write-Host "`nDownloading $name"
        Invoke-WebRequest -Uri $TensorRtUrl -OutFile $TensorRtZip -UseBasicParsing
    }
    if ($TensorRtZip) {
        if (-not (Test-Path $TensorRtZip)) { throw "TensorRT zip not found: $TensorRtZip" }
        Write-Host "`n=== TensorRT : unpacking $(Split-Path -Leaf $TensorRtZip) ===" -ForegroundColor Cyan
        Expand-Archive -Path $TensorRtZip -DestinationPath $thirdParty -Force
        $trtDir = Get-ChildItem $thirdParty -Directory -Filter 'TensorRT-*' |
            Sort-Object Name -Descending | Select-Object -First 1
    }
}
if ($trtDir) {
    Write-Host "`n=== TensorRT : $($trtDir.Name) ===" -ForegroundColor DarkGray
} else {
    Write-Warning @'
TensorRT was not found under third_party\, so the YOLO/TensorRT build stays off.
TensorRT is not published as a Windows binary wheel, so it needs the official
NVIDIA zip for the SAME version that built models\train13\weights\best.engine
(for TensorRT 11.3.0 that is the cuda-13.4 Windows zip, e.g.
 TensorRT-Enterprise-11.3.0.99-Windows-amd64-cuda-13.4-Release-external.zip
from developer.nvidia.com/tensorrt/download).  Then re-run either of:
  scripts\build_deps_windows.ps1 -TensorRtZip C:\path\to\<that zip>
  scripts\build_deps_windows.ps1 -TensorRtUrl <download url>
'@
}

Write-Host "`nThird-party prefix: $prefix" -ForegroundColor Green
Get-ChildItem (Join-Path $prefix 'bin') -Filter '*.dll' | Select-Object -ExpandProperty Name
if ($trtDir) { Write-Host "TensorRT: $($trtDir.FullName)" -ForegroundColor Green }
