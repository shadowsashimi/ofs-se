<#
.SYNOPSIS
    Build and run the group drag checks.

.DESCRIPTION
    Compiles tools\groupmove_probe.cpp against OFS-lib\Funscript\FunscriptGroupMove.h
    and runs it. Like tempo-probe.ps1 it stays out of the app build: the clamp
    that decides how far a dragged selection may travel is plain arithmetic,
    and a number answers "did the group jump a neighbour" faster and more
    reliably than dragging points around in the app.

    Exits with the probe's exit code, so a failing check fails whatever called
    this.

.EXAMPLE
    tools\groupmove-probe.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root   = Split-Path -Parent $PSScriptRoot
$OutDir = Join-Path $Root 'build\groupmove-probe'
$Source = Join-Path $PSScriptRoot 'groupmove_probe.cpp'

function Write-Fail([string] $Message) {
    Write-Host "!!! $Message" -ForegroundColor Red
}

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

# FunscriptAction.h brings in the binary serialisation header, which needs
# bitsery and, at its tail, imgui.h. OFS-lib\UI is there for OFS_Profiling.h.
$includeDirs = @(
    (Join-Path $Root 'OFS-lib'),
    (Join-Path $Root 'OFS-lib\UI'),
    (Join-Path $Root 'OFS-lib\Funscript'),
    (Join-Path $Root 'lib\bitsery\include'),
    (Join-Path $Root 'lib\imgui')
)
$includes = ($includeDirs | ForEach-Object { "/I `"$_`"" }) -join ' '

# Built from inside the output directory rather than with /Fo, because a quoted
# directory path ending in a backslash escapes its own closing quote.
$build = "`"$vcvars`" >nul && cd /d `"$OutDir`" && " +
    "cl /nologo /std:c++17 /EHsc /O2 /W3 $includes /Fe:groupmove_probe.exe `"$Source`""

$output = & cmd /c $build 2>&1
if ($LASTEXITCODE -ne 0) {
    $output | ForEach-Object { Write-Host "  $_" }
    Write-Fail 'Build failed'
    exit 1
}

& (Join-Path $OutDir 'groupmove_probe.exe')
exit $LASTEXITCODE
