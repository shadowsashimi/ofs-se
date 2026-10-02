<#
.SYNOPSIS
    Run the UI tests that cover what has changed, several at a time.

.DESCRIPTION
    By default only the scripts that exercise the files you have changed are
    run: uncommitted changes, or everything since -Since. Each run starts its
    own copy of the app, so running only what is relevant is what keeps this
    quick. -All runs everything, -Filter picks scripts by name.

    Scripts say what they cover with lines near their top:

      #! covers src/UI/OFS_ChapterManager.* OFS-lib/Funscript/*
                           repo paths, wildcards allowed. PATH:NAME covers
                           only changes inside function NAME of that file, for
                           big files like src/OpenFunscripter.cpp, going by
                           the function git names in each diff hunk
      #! media mp3 m4a     run once per named media (see ui-test.ps1) instead
                           of once with the video; results go to <name>-<media>
      #! sweep             mostly screenshots for a person to look at, often
                           long; only run with -Sweeps or -All
      #! server <name>     run tools\<name>.ps1 alongside the app, for a
                           script that needs something to talk to

    A few files change what every script sees (the theme moves every click,
    the driver runs every line), so touching one of those selects every
    script. A changed file no script covers selects smoke, and is listed so
    the gap is visible. Changes outside source and test files select nothing.

    Build first: tools\dev-run.ps1 -NoRun -NoKill

    Exits 0 only when every run passed.

.EXAMPLE
    tools\ui-test-all.ps1
    The scripts covering your uncommitted changes.

.EXAMPLE
    tools\ui-test-all.ps1 -Since origin/ofs-se -List
    What covers everything not yet pushed, without running it.

.EXAMPLE
    tools\ui-test-all.ps1 -Filter chapter* -Sweeps
    Scripts by name, sweeps included.

.EXAMPLE
    tools\ui-test-all.ps1 -All
    Everything.
#>
[CmdletBinding()]
param(
    # Compare against this commit instead of HEAD, so committed work counts too.
    [string] $Since = 'HEAD',
    # Compare -Since with this commit instead of the working tree, to ask what
    # covers one commit or a range: -Since HEAD~1 -Until HEAD.
    [string] $Until,
    # Script names to run, wildcards allowed, instead of choosing by changes.
    [string[]] $Filter,
    # Every script, sweeps included.
    [switch] $All,
    # Include sweeps among the scripts chosen.
    [switch] $Sweeps,
    # Say what would run and why, and stop.
    [switch] $List,
    # Runs at once.
    [int] $Jobs = 4,
    [ValidateSet('Release', 'Debug')] [string] $Config = 'Release'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = Split-Path -Parent $PSScriptRoot
$Runner = Join-Path $PSScriptRoot 'ui-test.ps1'
$Exe = Join-Path $Root "bin\$Config\OFS-SE.exe"

# Changes that alter what every script sees: the theme moves every click, and
# these parts of the driver carry out every line. The rest of the driver only
# reports state, which smoke covers.
$Everything = @(
    'OFS-lib/UI/OFS_SashimiTheme.*'
    'src/OFS_UiDriver.cpp:Enabled'
    'src/OFS_UiDriver.cpp:FromEnvironment'
    'src/OFS_UiDriver.cpp:parseButton'
    'src/OFS_UiDriver.cpp:parseModifier'
    'src/OFS_UiDriver.cpp:parseKey'
    'src/OFS_UiDriver.cpp:parse'
    'src/OFS_UiDriver.cpp:holdWindowSize'
    'src/OFS_UiDriver.cpp:BeforeNewFrame'
    'src/OFS_UiDriver.cpp:execute'
    'src/OFS_UiDriver.cpp:conditionMet'
    'src/OFS_UiDriver.cpp:AfterRender'
    'tools/ui-test.ps1'
    'OFS-lib/imgui_impl/*'
    'lib/imgui/*'
    'src/OpenFunscripter.cpp:setupDefaultLayout'
)
# Only these kinds of change can break a script.
$Relevant = @('*.cpp', '*.c', '*.h', '*.hpp', '*.inl', '*.glsl', '*.lua', 'CMakeLists.txt', 'tools/ui-test.ps1', 'tools/uitest/*.txt')

# ------------------------------------------------------------- scripts ----
$scripts = @(foreach ($file in Get-ChildItem (Join-Path $PSScriptRoot 'uitest') -Filter '*.txt' | Sort-Object Name) {
    $sweep = $false
    $media = @()
    $covers = @()
    foreach ($line in Get-Content $file.FullName -TotalCount 30) {
        if ($line -match '^#!\s*sweep\b') { $sweep = $true }
        elseif ($line -match '^#!\s*media\s+(.+)$') { $media = @($Matches[1].Trim() -split '\s+') }
        elseif ($line -match '^#!\s*covers\s+(.+)$') { $covers += @($Matches[1].Trim() -split '\s+') }
    }
    [pscustomobject]@{ Name = $file.BaseName; Path = $file.FullName; Sweep = $sweep; Media = $media; Covers = $covers; Why = '' }
})

# ------------------------------------------------------------- changes ----
# A covers entry naming a function matches a file when one of the file's diff
# hunks is inside that function; one without matches any change to the file.
function Test-Covers([string] $entry, $change) {
    $path, $fn = $entry -split ':', 2
    if ($change.Path -notlike $path) { return $false }
    if (-not $fn) { return $true }
    return $change.Functions -contains $fn
}

function Test-CoversAny([string[]] $entries, $change) {
    foreach ($e in $entries) { if (Test-Covers $e $change) { return $true } }
    return $false
}

function Get-Changes {
    Push-Location $Root
    try {
        $changes = [ordered]@{}
        $current = $null
        # Built up rather than returned from an if, which would unwrap a lone
        # element into a string and splat it one character at a time.
        $range = @($Since)
        if ($Until) { $range += $Until }
        foreach ($line in (& git -c core.safecrlf=false diff -U0 --no-color @range -- 2>$null)) {
            if ($line -match '^diff --git a/(.+) b/(.+)$') {
                $current = [pscustomobject]@{ Path = $Matches[2]; Functions = [Collections.Generic.List[string]]::new() }
                $changes[$current.Path] = $current
            }
            elseif ($current -and $line -match '^@@[^@]*@@\s*(.*)$') {
                # Git heads each hunk with the nearest line above it that looks
                # like the start of a function; the name is the identifier
                # just before that line's first "(".
                $header = $Matches[1]
                $name = if ($header -match '([A-Za-z_]\w*)\s*\(') { $Matches[1] } else { '' }
                $current.Functions.Add($name)
            }
        }
        if ($LASTEXITCODE -ne 0) { throw "git diff $($range -join ' ') failed" }
        $untracked = if ($Until) { @() } else { @(& git -c core.safecrlf=false ls-files --others --exclude-standard 2>$null) }
        foreach ($path in $untracked) {
            if (-not $changes.Contains($path)) {
                $changes[$path] = [pscustomobject]@{ Path = $path; Functions = [Collections.Generic.List[string]]::new() }
            }
        }
        return @($changes.Values | Where-Object { Test-CoversAny $Relevant $_ })
    }
    finally { Pop-Location }
}

$changes = @()
$uncovered = @()
$skippedSweeps = @()

if ($All) {
    foreach ($s in $scripts) { $s.Why = 'all' }
    $chosen = $scripts
}
elseif ($Filter) {
    $chosen = @($scripts | Where-Object { $n = $_.Name; @($Filter | Where-Object { $n -like $_ }).Count -gt 0 })
    foreach ($s in $chosen) { $s.Why = 'by name' }
    if (-not $Sweeps) {
        $skippedSweeps = @($chosen | Where-Object Sweep)
        $chosen = @($chosen | Where-Object { -not $_.Sweep })
    }
}
else {
    $changes = @(Get-Changes)
    if ($changes.Count -eq 0) {
        Write-Host "==> No source or test changes since $Since, so nothing to run. -All runs everything." -ForegroundColor Cyan
        exit 0
    }

    $broad = @($changes | Where-Object { Test-CoversAny $Everything $_ })
    foreach ($s in $scripts) {
        $reasons = [Collections.Generic.List[string]]::new()
        if ($broad.Count -gt 0) { $reasons.Add("$($broad[0].Path) affects every script") }
        foreach ($c in $changes) {
            if ($c.Path -eq "tools/uitest/$($s.Name).txt") { $reasons.Add('script changed') }
            elseif (Test-CoversAny $s.Covers $c) { $reasons.Add($c.Path) }
        }
        if ($reasons.Count -gt 0) { $s.Why = (@($reasons) | Select-Object -Unique) -join ', ' }
    }

    $uncovered = @(foreach ($c in $changes) {
        if ($c.Path -like 'tools/uitest/*.txt') { continue }
        if (Test-CoversAny $Everything $c) { continue }
        if (@($scripts | Where-Object { Test-CoversAny $_.Covers $c }).Count -gt 0) { continue }
        $fns = @($c.Functions | Where-Object { $_ } | Select-Object -Unique)
        if ($fns.Count -gt 0) { "$($c.Path) ($($fns -join ', '))" } else { $c.Path }
    })
    if ($uncovered.Count -gt 0) {
        $smoke = $scripts | Where-Object Name -eq 'smoke'
        if ($smoke -and -not $smoke.Why) { $smoke.Why = 'nothing else covers a change' }
    }

    $chosen = @($scripts | Where-Object { $_.Why })
    if (-not $Sweeps) {
        $skippedSweeps = @($chosen | Where-Object Sweep)
        $chosen = @($chosen | Where-Object { -not $_.Sweep })
    }
}

$runs = @(foreach ($s in $chosen) {
    $timeout = if ($s.Sweep) { 900 } else { 300 }
    $mediaList = if ($s.Media.Count -gt 0) { $s.Media } else { @('video') }
    foreach ($m in $mediaList) {
        $run = if ($s.Media.Count -gt 0) { "$($s.Name)-$m" } else { $s.Name }
        [pscustomobject]@{ Run = $run; Script = $s.Path; Media = $m; Sweep = $s.Sweep; Timeout = $timeout; Why = $s.Why }
    }
})

# --------------------------------------------------------------- report ----
if ($changes.Count -gt 0) {
    $span = if ($Until) { "$Since..$Until" } else { "since $Since" }
    Write-Host "==> $($changes.Count) changed file(s) $span" -ForegroundColor Cyan
}
foreach ($r in $runs) {
    Write-Host ('    {0,-22} {1}' -f $r.Run, $r.Why) -ForegroundColor DarkGray
}
if ($skippedSweeps.Count -gt 0) {
    Write-Host "    sweeps also chosen, left out without -Sweeps: $(@($skippedSweeps.Name) -join ', ')" -ForegroundColor DarkGray
}
if ($uncovered.Count -gt 0) {
    Write-Host '    no script covers:' -ForegroundColor Yellow
    foreach ($u in $uncovered) { Write-Host "      $u" -ForegroundColor Yellow }
}
if ($runs.Count -eq 0) {
    Write-Host '==> Nothing to run.' -ForegroundColor Cyan
    exit 0
}
if ($List) { exit 0 }

# ---------------------------------------------------------------- build ----
if (-not (Test-Path $Exe)) { throw "Not built: $Exe. Run tools\dev-run.ps1 -NoRun -NoKill first." }
$built = (Get-Item $Exe).LastWriteTime
$newer = @(Get-ChildItem (Join-Path $Root 'src'), (Join-Path $Root 'OFS-lib') -Recurse -File -Include '*.cpp', '*.h' |
    Where-Object { $_.LastWriteTime -gt $built })
if ($newer.Count -gt 0) {
    Write-Host "!!! $($newer.Count) source file(s) are newer than the build, $($newer[0].Name) among them. Run tools\dev-run.ps1 -NoRun -NoKill first." -ForegroundColor Yellow
}

# ------------------------------------------------------------------ run ----
# Made one at a time here, so parallel runs never race to write the same file.
foreach ($m in @($runs.Media | Sort-Object -Unique)) {
    & pwsh -NoProfile -File $Runner -Media $m -PrepareOnly
    if ($LASTEXITCODE -ne 0) { throw "Could not prepare media '$m'" }
}

$jobsNow = [math]::Min($Jobs, $runs.Count)
Write-Host "==> $($runs.Count) run(s), $jobsNow at a time" -ForegroundColor Cyan
$watch = [Diagnostics.Stopwatch]::StartNew()

# Long ones first, so they are not the last still going.
$ordered = @($runs | Sort-Object @{ Expression = 'Sweep'; Descending = $true }, Run)
$results = @($ordered | ForEach-Object -ThrottleLimit $jobsNow -Parallel {
    $run = $_
    $out = Join-Path $using:Root "build\uitest\$($run.Run)"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $lines = & pwsh -NoProfile -File $using:Runner -Script $run.Script -Media $run.Media `
        -Out $out -TimeoutSeconds $run.Timeout -Config $using:Config 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $sw.Stop()

    $summary = @($lines | Where-Object { $_ -match 'done:' }) | Select-Object -Last 1
    $result = [pscustomobject]@{
        Run = $run.Run
        Passed = ($code -eq 0)
        Seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1)
        Summary = if ($summary) { ($summary -replace '^\[\s*\d+\]\s*', '') } else { 'did not finish' }
        Problems = @($lines | Where-Object { $_ -cmatch ': FAIL ' -or $_ -match '^!!!' -or $_ -match 'Exception' })
        Out = $out
    }
    $colour = if ($result.Passed) { 'Green' } else { 'Red' }
    $label = if ($result.Passed) { 'PASS' } else { 'FAIL' }
    Write-Host ('{0}  {1,-22} {2,6:N1}s  {3}' -f $label, $result.Run, $result.Seconds, $result.Summary) -ForegroundColor $colour
    $result
})
$watch.Stop()

$failed = @($results | Where-Object { -not $_.Passed })
if ($failed.Count -gt 0) {
    Write-Host ''
    foreach ($f in $failed) {
        Write-Host "--- $($f.Run)  ($($f.Out))" -ForegroundColor Red
        foreach ($p in $f.Problems) { Write-Host "    $p" -ForegroundColor Red }
    }
    Write-Host ''
    Write-Host ("!!! {0} of {1} run(s) failed, in {2:N0}s" -f $failed.Count, $results.Count, $watch.Elapsed.TotalSeconds) -ForegroundColor Red
    exit 1
}
Write-Host ("==> {0} run(s) passed in {1:N0}s" -f $results.Count, $watch.Elapsed.TotalSeconds) -ForegroundColor Cyan
exit 0
