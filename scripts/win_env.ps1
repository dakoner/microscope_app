#Requires -Version 5.1
<#
.SYNOPSIS
    Loads the Visual Studio (MSVC) developer environment plus the Qt and Python
    virtual environment paths into the current PowerShell session.

.DESCRIPTION
    Used by the Windows build scripts so that cmake, ninja, cl, nmake, qmake and
    the project virtual environment are all available on PATH.
#>

$ErrorActionPreference = 'Stop'

function Enter-MsvcDevEnvironment {
    param([string]$Arch = 'amd64')

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw 'Visual Studio Installer (vswhere.exe) not found. Install Visual Studio 2022 Build Tools.'
    }

    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) {
        # Fall back to any installed instance (Build Tools without explicit workload metadata).
        $vsPath = & $vswhere -latest -products * -property installationPath
    }
    if (-not $vsPath) { throw 'No Visual Studio installation with the C++ toolchain was found.' }

    $launch = Join-Path $vsPath 'Common7\Tools\Launch-VsDevShell.ps1'
    if (Test-Path $launch) {
        & $launch -Arch $Arch -HostArch $Arch -SkipAutomaticLocation | Out-Null
    } else {
        $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
        if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found below $vsPath" }
        cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
            if ($_ -match '^(.*?)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
        }
    }
}

function Get-QtPrefixPath {
    param([string]$Preferred = 'C:\Qt\6.11.2\msvc2022_64')

    if (Test-Path (Join-Path $Preferred 'lib\cmake\Qt6')) { return (Resolve-Path $Preferred).Path }

    $qtRoot = 'C:\Qt'
    if (Test-Path $qtRoot) {
        $kits = Get-ChildItem $qtRoot -Directory |
            Where-Object { $_.Name -like '6.*' } |
            Sort-Object Name -Descending
        foreach ($kit in $kits) {
            foreach ($arch in 'msvc2022_64', 'msvc2019_64') {
                $candidate = Join-Path $kit.FullName $arch
                if (Test-Path (Join-Path $candidate 'lib\cmake\Qt6')) { return $candidate }
            }
        }
    }
    throw 'No Qt 6 MSVC kit with CMake configuration files was found under C:\Qt.'
}

function Enter-MicroscopeEnvironment {
    param([string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot))

    $ProjectRoot = (Resolve-Path $ProjectRoot).Path
    Enter-MsvcDevEnvironment

    $qtPrefix = Get-QtPrefixPath
    $qtBin = Join-Path $qtPrefix 'bin'
    $env:QT_PREFIX_PATH = $qtPrefix

    $ninjaDir = 'C:\Qt\Tools\Ninja'
    $cmakeDir = 'C:\Qt\Tools\CMake_64\bin'

    $pathParts = @()
    if (Test-Path $cmakeDir) { $pathParts += $cmakeDir }
    if (Test-Path $ninjaDir) { $pathParts += $ninjaDir }
    $pathParts += $qtBin
    $pathParts += (Join-Path $ProjectRoot '.venv\Scripts')
    $env:PATH = (($pathParts + @($env:PATH -split ';') | Where-Object { $_ }) -join ';')

    $venvPython = Join-Path $ProjectRoot '.venv\Scripts\python.exe'
    if (-not (Test-Path $venvPython)) { throw "Project virtual environment missing: $venvPython" }
    $env:MICROSCOPE_VENV_PYTHON = $venvPython

    $clCmd = Get-Command cl.exe -ErrorAction SilentlyContinue
    Write-Host "MSVC compiler : $(if ($clCmd) { $clCmd.Source } else { 'NOT FOUND' })"
    Write-Host "Qt kit        : $qtPrefix"
    Write-Host "Python (venv) : $venvPython"
}
