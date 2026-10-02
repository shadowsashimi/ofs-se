<#
.SYNOPSIS
    Build and run the tempo detection checks.

.DESCRIPTION
    Compiles tools\tempo_probe.cpp against OFS-lib\OFS_TempoDetection.cpp and
    runs it. Kept apart from the app build on purpose: the detector needs
    nothing beyond the standard library, so this builds in seconds where a
    round trip through the app takes minutes, and it answers "is the tempo
    right" with a number instead of a screenshot.

    Exits with the probe's exit code, so a failing check fails whatever called
    this.

.EXAMPLE
    tools\tempo-probe.ps1
    Build and run against the current detector.

.EXAMPLE
    git show HEAD~1:OFS-lib/OFS_TempoDetection.cpp > $env:TEMP\old.cpp
    tools\tempo-probe.ps1 -Detector $env:TEMP\old.cpp
    Run the same checks against an older detector, to confirm a check actually
    catches the bug it was written for.
#>
[CmdletBinding()]
param(
    # Detector source to compile instead of the one in the tree.
    [string] $Detector
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root   = Split-Path -Parent $PSScriptRoot
$OutDir = Join-Path $Root 'build\tempo-probe'
$Source = Join-Path $PSScriptRoot 'tempo_probe.cpp'
if (-not $Detector) { $Detector = Join-Path $Root 'OFS-lib\OFS_TempoDetection.cpp' }
$Detector = (Resolve-Path $Detector).Path

function Write-Fail([string] $Message) {
    Write-Host "!!! $Message" -ForegroundColor Red
}

# vswhere ships with every Visual Studio install from 2017 on, and is the
# supported way to find one without guessing at edition and install paths.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    Write-Fail 'vswhere.exe not found; is Visual Studio or its Build Tools installed?'
    exit 1
}
$install = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $install) {
    Write-Fail 'No Visual Studio install with the C++ x64 toolset was found.'
    exit 1
}
$vcvars = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# OFS-lib\UI is only there for OFS_Profiling.h, whose macro compiles to nothing
# without OFS_PROFILE_ENABLED.
$includes = "/I `"$(Join-Path $Root 'OFS-lib')`" /I `"$(Join-Path $Root 'OFS-lib\UI')`""

# Built from inside the output directory rather than with /Fo, because a quoted
# directory path ending in a backslash escapes its own closing quote.
$build = "`"$vcvars`" >nul && cd /d `"$OutDir`" && " +
    "cl /nologo /std:c++17 /EHsc /O2 /W3 $includes /Fe:tempo_probe.exe `"$Source`" `"$Detector`""

$output = & cmd /c $build 2>&1
if ($LASTEXITCODE -ne 0) {
    $output | ForEach-Object { Write-Host "  $_" }
    Write-Fail 'Build failed'
    exit 1
}

& (Join-Path $OutDir 'tempo_probe.exe')
exit $LASTEXITCODE
