<#
.SYNOPSIS
    Close the running OFS build, rebuild it incrementally, and relaunch it.

.DESCRIPTION
    The dev loop for OFS is bottlenecked by the exe being locked while it runs,
    so a rebuild fails at link with LNK1104 until you close the app by hand.
    This does that for you: graceful close -> incremental build -> relaunch.

    The close is graceful on purpose, because the two things OFS persists have
    very different exposure to a hard kill:

      imgui.ini      docking layout and window geometry. ImGui autosaves this
                     every IniSavingRate seconds (5 by default), so a kill
                     costs at most the last few seconds of layout fiddling.
      OFS state JSON panel visibility, preferences, simulator settings. Written
                     only by SaveState() in OpenFunscripter::Shutdown(), so a
                     kill discards every change made since the app started.

    A graceful close normally completes in about 1.5s. The exception is a
    project with unsaved edits: exitApp() raises a native Yes/No/Cancel dialog
    that blocks until a human answers it, and no amount of waiting will clear
    it. That is what -CloseTimeout bounds before falling back to a force kill.

    Only an instance launched from THIS tree's bin directory is closed, so a
    build running out of another worktree is left alone.

.EXAMPLE
    tools\dev-run.ps1
    Close, build Release, relaunch.

.EXAMPLE
    tools\dev-run.ps1 -NoRun
    Close and build, but do not relaunch. Useful as a syntax check.

.EXAMPLE
    tools\dev-run.ps1 -Config Debug
    Same loop against the Debug configuration.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string] $Config = 'Release',

    # CMake target to build. The exe is expected at bin/<Config>/<Target>.exe.
    [string] $Target = 'OFS-SE',

    # Build but do not relaunch.
    [switch] $NoRun,

    # Leave a running instance alone. The build will fail at link if one holds
    # the exe; use this when you only want to compile.
    [switch] $NoKill,

    # Delete the build directory and configure from scratch first.
    [switch] $Clean,

    # Seconds to wait for a graceful exit before force killing.
    [int] $CloseTimeout = 6,

    # Arguments forwarded to the relaunched application.
    [string[]] $AppArgs = @()
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root     = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root 'build'
$ExePath  = Join-Path $Root "bin\$Config\$Target.exe"
$LogPath  = Join-Path $BuildDir "dev-run-$Config.log"

function Write-Step([string] $Message) {
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Write-Fail([string] $Message) {
    Write-Host "!!! $Message" -ForegroundColor Red
}

$totalWatch = [Diagnostics.Stopwatch]::StartNew()

# ---------------------------------------------------------------- close ----
# Match on the executable path rather than the process name, so instances
# started from a different worktree or an installed copy are left untouched.
if (-not $NoKill) {
    $running = @(
        Get-Process -Name $Target -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -and ($_.Path -eq $ExePath) }
    )

    if ($running.Count -gt 0) {
        Write-Step "Closing $($running.Count) running instance(s)"

        foreach ($proc in $running) {
            # CloseMainWindow posts WM_CLOSE, which lets OFS run its normal
            # shutdown path and persist state. It returns false for a process
            # with no main window, in which case there is nothing to be polite
            # about and the force kill below covers it.
            $null = $proc.CloseMainWindow()
        }

        foreach ($proc in $running) {
            if (-not $proc.WaitForExit($CloseTimeout * 1000)) {
                # Almost always the unsaved-changes dialog: exitApp() blocks on a
                # native prompt that nothing here can answer. Say so, because the
                # fix is to save the project, not to raise -CloseTimeout.
                Write-Fail "PID $($proc.Id) did not exit within ${CloseTimeout}s - force killing."
                Write-Fail '    Usually an unsaved-changes prompt. Preferences and panel'
                Write-Fail '    visibility from this session will be lost (imgui.ini layout survives).'
                Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
                $null = $proc.WaitForExit(3000)
            }
        }

        # The handle on the exe is released slightly after the process object
        # reports exit, so linking immediately can still hit LNK1104. Confirm
        # the file is actually writable before starting the build.
        $deadline = (Get-Date).AddSeconds(5)
        while ((Test-Path $ExePath) -and ((Get-Date) -lt $deadline)) {
            try {
                $handle = [IO.File]::Open($ExePath, 'Open', 'ReadWrite', 'None')
                $handle.Close()
                break
            }
            catch {
                Start-Sleep -Milliseconds 100
            }
        }
    }
}

# ------------------------------------------------------------ configure ----
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Step 'Removing build directory'
    Remove-Item -Recurse -Force $BuildDir
}

# A configure that fails partway still leaves a CMakeCache.txt behind, so the
# cache alone is not evidence of a usable build tree. Require the generated
# solution too, otherwise a half-configured directory is silently treated as
# ready and the build dies with an unhelpful MSB1009.
$isConfigured = (Test-Path (Join-Path $BuildDir 'CMakeCache.txt')) -and
    (@(Get-ChildItem -Path $BuildDir -Filter '*.sln' -ErrorAction SilentlyContinue).Count -gt 0)

if (-not $isConfigured) {
    Write-Step 'Configuring (no usable build tree found)'
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

    # src/CMakeLists.txt downloads ~80MB of libmpv from SourceForge at configure
    # time, but only when build/mpv-2.dll is absent. Copying it across from a
    # sibling worktree's build directory skips a slow and flaky download.
    if (-not (Test-Path (Join-Path $BuildDir 'mpv-2.dll'))) {
        $commonDir = (& git -C $Root rev-parse --path-format=absolute --git-common-dir 2>$null)
        if ($LASTEXITCODE -eq 0 -and $commonDir) {
            $seed = Join-Path (Split-Path -Parent $commonDir) 'build\mpv-2.dll'
            if (Test-Path $seed) {
                Write-Step 'Seeding mpv-2.dll from the main checkout'
                Copy-Item $seed (Join-Path $BuildDir 'mpv-2.dll')
            }
        }
    }

    # Six of the vendored libraries under lib/ declare a cmake_minimum_required
    # below 3.5 (glm, bitsery, eventpp, json, civetweb, SDL2), and CMake 4.x
    # removed compatibility with those. The existing build tree was configured
    # with this same policy override, so match it rather than editing vendored
    # CMakeLists files.
    # The -D value must stay quoted. Unquoted, PowerShell parses the trailing
    # "=3.5" as an expression and passes two arguments ("...=3" and ".5"), which
    # CMake rejects as an invalid policy version.
    & cmake -S $Root -B $BuildDir -G 'Visual Studio 17 2022' -A x64 '-DCMAKE_POLICY_VERSION_MINIMUM=3.5'
    if ($LASTEXITCODE -ne 0) {
        # Drop the partial cache so the next run configures from scratch instead
        # of inheriting whatever state this failure left behind.
        Remove-Item (Join-Path $BuildDir 'CMakeCache.txt') -Force -ErrorAction SilentlyContinue
        Write-Fail 'Configure failed'
        exit 1
    }
}

# ---------------------------------------------------------------- build ----
Write-Step "Building $Target ($Config)"
$buildWatch = [Diagnostics.Stopwatch]::StartNew()

$output = & cmake --build $BuildDir --config $Config --target $Target --parallel 2>&1
$buildOk = ($LASTEXITCODE -eq 0)
$buildWatch.Stop()

$output | Out-File -FilePath $LogPath -Encoding utf8

if (-not $buildOk) {
    # The build emits several thousand lines of C4244 conversion warnings, so
    # echoing raw output buries the actual failure. Show only diagnostics that
    # stopped the build; the unfiltered log stays on disk.
    Write-Fail "Build failed after $([math]::Round($buildWatch.Elapsed.TotalSeconds, 1))s"

    $errors = @(
        $output |
            Select-String -Pattern '(: (fatal )?error [A-Z]+\d+)|(^LINK : fatal)|(MSBUILD : error)' |
            Select-Object -ExpandProperty Line -Unique
    )

    Write-Host ''
    if ($errors.Count -gt 0) {
        $errors | ForEach-Object { Write-Host "  $($_.Trim())" -ForegroundColor Red }
    }
    else {
        # Nothing matched the error patterns, so fall back to the tail rather
        # than reporting a failure with nothing to show for it.
        $output | Select-Object -Last 20 | ForEach-Object { Write-Host "  $_" }
    }

    Write-Host ''
    Write-Host "  full log: $LogPath" -ForegroundColor DarkGray
    exit 1
}

Write-Host "    built in $([math]::Round($buildWatch.Elapsed.TotalSeconds, 1))s" -ForegroundColor DarkGray

# --------------------------------------------------------------- launch ----
if ($NoRun) {
    $totalWatch.Stop()
    Write-Step "Done in $([math]::Round($totalWatch.Elapsed.TotalSeconds, 1))s (not relaunched)"
    exit 0
}

if (-not (Test-Path $ExePath)) {
    Write-Fail "Build reported success but $ExePath is missing"
    exit 1
}

Write-Step 'Relaunching'
$startArgs = @{
    FilePath         = $ExePath
    WorkingDirectory = (Split-Path -Parent $ExePath)
}
if ($AppArgs.Count -gt 0) {
    $startArgs['ArgumentList'] = $AppArgs
}
Start-Process @startArgs

$totalWatch.Stop()
Write-Step "Done in $([math]::Round($totalWatch.Elapsed.TotalSeconds, 1))s"
