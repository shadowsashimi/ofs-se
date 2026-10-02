<#
.SYNOPSIS
    Run a UI test script against the built app, in a throwaway profile.

.DESCRIPTION
    Launches bin\<Config>\OFS-SE.exe with OFS_UITEST_SCRIPT set, which makes the
    app play the script frame by frame and write screenshots, state dumps and
    a driver.log of PASS and FAIL lines to the output directory. See
    src\OFS_UiDriver.h for the script commands.

    The app runs with OFS_PREF_PATH pointing at a fresh directory under TEMP,
    so the run starts with default layout and settings and cannot touch the
    real profile, its recent files, or its backups. ffmpeg is hard linked in
    from the real profile when it is there, so the waveform works and the
    download prompt stays away.

    Each run gets its own profile and its own hard linked copy of the media,
    both named after the output directory, so runs can go side by side and a
    project one test saves beside its media is never seen by another.
    tools\ui-test-all.ps1 runs the whole suite that way.

    -Media is a file, or the name of one generated with that ffmpeg:
      video      a minute of test pattern with a click every half second,
                 which is 120 BPM for the tempo tools (the default)
      mp3, m4a   the same clicks as audio alone
      ramp       the same clicks as audio, getting louder over the minute
      mp3-caps   the mp3 with its name in capitals
      m4a-cover  the m4a with cover art, which mpv shows as a still picture
      dotted     the video named with dots of its own, 'Site.COM - clip.mp4'

    Exits 0 only when the script ran to the end with no FAIL lines.

.EXAMPLE
    tools\ui-test.ps1 tools\uitest\smoke.txt

.EXAMPLE
    tools\ui-test.ps1 tools\uitest\audio.txt -Media m4a -Out build\uitest\audio-m4a
#>
[CmdletBinding()]
param(
    [string] $Script,
    [string] $Out,
    [string] $Media = 'video',
    [int] $TimeoutSeconds = 240,
    [ValidateSet('Release', 'Debug')] [string] $Config = 'Release',
    # Make the named media if it is missing, then stop. ui-test-all.ps1 does
    # this once up front, rather than have several runs make it at once.
    [switch] $PrepareOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = Split-Path -Parent $PSScriptRoot
$Exe = Join-Path $Root "bin\$Config\OFS-SE.exe"
$MediaRoot = Join-Path $Root 'build\uitest\media'
$realFfmpeg = Join-Path $env:APPDATA 'OFS\OFS3_data\ffmpeg.exe'

# The click track every generated file shares.
$clicks = "aevalsrc='0.8*sin(2*PI*880*t)*exp(-40*mod(t,0.5))':s=44100:d=60"
$generated = [ordered]@{
    'video'     = @{ File = 'uitest.mp4'; Args = @(
        '-f', 'lavfi', '-i', 'testsrc2=size=1280x720:rate=30:duration=60',
        '-f', 'lavfi', '-i', $clicks,
        '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-preset', 'veryfast', '-c:a', 'aac', '-shortest') }
    'mp3'       = @{ File = 'tone.mp3'; Args = @('-f', 'lavfi', '-i', $clicks, '-c:a', 'libmp3lame', '-q:a', '4') }
    'm4a'       = @{ File = 'tone.m4a'; Args = @('-f', 'lavfi', '-i', $clicks, '-c:a', 'aac') }
    # The same beat getting steadily louder, for anything that follows loudness.
    'ramp'      = @{ File = 'ramp.mp3'; Args = @(
        '-f', 'lavfi', '-i', "aevalsrc='0.9*sin(2*PI*880*t)*exp(-40*mod(t,0.5))*(0.1+0.9*t/60)':s=44100:d=60",
        '-c:a', 'libmp3lame', '-q:a', '4') }
    'mp3-caps'  = @{ File = 'CAPS.MP3'; From = 'mp3' }
    'm4a-cover' = @{ File = 'cover.m4a'; From = 'm4a'; Cover = $true }
    # The video under a name with dots of its own, as downloads often have.
    'dotted'    = @{ File = 'Site.COM - clip.mp4'; From = 'video' }
}

function Get-GeneratedMedia([string] $name) {
    $spec = $generated[$name]
    $path = Join-Path $MediaRoot $spec.File
    if (Test-Path $path) { return $path }

    if (-not (Test-Path $realFfmpeg)) { throw "No test media '$name' and no ffmpeg to make it with; pass -Media" }
    New-Item -ItemType Directory -Force $MediaRoot | Out-Null
    $ff = @('-hide_banner', '-loglevel', 'error', '-y')

    if ($spec.ContainsKey('Cover')) {
        $source = Get-GeneratedMedia $spec.From
        $picture = Join-Path $MediaRoot 'cover.png'
        & $realFfmpeg @ff -f lavfi -i 'testsrc2=size=500x500:duration=1' -frames:v 1 $picture
        & $realFfmpeg @ff -i $source -i $picture -map 0 -map 1 -c copy -disposition:v attached_pic $path
        Remove-Item $picture -ErrorAction SilentlyContinue
    }
    elseif ($spec.ContainsKey('From')) {
        Copy-Item (Get-GeneratedMedia $spec.From) $path
    }
    else {
        $ffArgs = $spec.Args
        & $realFfmpeg @ff @ffArgs $path
    }
    if (-not (Test-Path $path)) { throw "Generating test media '$name' failed" }
    return $path
}

if ($generated.Contains($Media)) {
    $Media = Get-GeneratedMedia $Media
}
elseif (-not (Test-Path $Media)) {
    throw "No such media: $Media (a file, or one of: $($generated.Keys -join ', '))"
}
$Media = (Resolve-Path $Media).Path
if ($PrepareOnly) { exit 0 }

if (-not $Script) { throw 'Pass -Script' }
if (-not (Test-Path $Exe)) { throw "Not built: $Exe" }
$Script = (Resolve-Path $Script).Path
$Name = [IO.Path]::GetFileNameWithoutExtension($Script)
if (-not $Out) { $Out = Join-Path $Root "build\uitest\$Name" }

if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
# Absolute, because the app runs from bin and scripts use it as $OUT.
$Out = (Resolve-Path $Out).Path
# Everything else this run owns is named after its output directory, which is
# what tells two runs of one script with different media apart.
$RunName = Split-Path $Out -Leaf

# A private copy of the media, since a test may save a project beside it and
# any later test would then open that project in its place. Hard linked,
# because the video is twenty megabytes and there are dozens of runs.
$runMediaDir = Join-Path $Root "build\uitest\.runmedia\$RunName"
if (Test-Path $runMediaDir) { Remove-Item -Recurse -Force $runMediaDir }
New-Item -ItemType Directory -Force $runMediaDir | Out-Null
$runMedia = Join-Path $runMediaDir (Split-Path $Media -Leaf)
try { New-Item -ItemType HardLink -Path $runMedia -Target $Media | Out-Null }
catch { Copy-Item $Media $runMedia }
$Media = $runMedia

# On the same volume as the real profile, so ffmpeg can be hard linked rather
# than copied: it is a hundred megabytes.
$Profile = Join-Path $env:TEMP "ofs-uitest\$RunName"
if (Test-Path $Profile) { Remove-Item -Recurse -Force $Profile }
New-Item -ItemType Directory -Force $Profile | Out-Null
if (Test-Path $realFfmpeg) {
    $linked = Join-Path $Profile 'ffmpeg.exe'
    try { New-Item -ItemType HardLink -Path $linked -Target $realFfmpeg | Out-Null }
    catch { Copy-Item $realFfmpeg $linked }
}

$env:OFS_PREF_PATH = $Profile
$env:OFS_UITEST_SCRIPT = $Script
$env:OFS_UITEST_OUT = $Out
$env:OFS_UITEST_MEDIA = $Media

# A script can ask for a helper to be running while it plays, with a line of
# "#! server <name>" near its top: tools\<name>.ps1 is started before the app
# and stopped after it, and writes its log beside the results. mock-intiface is
# the one that exists, standing in for Intiface Central.
$helper = $null
$helperMatch = Select-String -Path $Script -Pattern '^#!\s*server\s+(\S+)\s*$' | Select-Object -First 1
if ($helperMatch) {
    $helperName = $helperMatch.Matches.Groups[1].Value
    $helperScript = Join-Path $PSScriptRoot "$helperName.ps1"
    if (-not (Test-Path $helperScript)) { throw "No such helper: $helperScript" }
    $helper = Start-Process -FilePath 'pwsh' -PassThru -WindowStyle Hidden -ArgumentList @(
        '-NoProfile', '-File', $helperScript,
        '-Log', (Join-Path $Out "$helperName.log"),
        '-TimeoutSeconds', [string]($TimeoutSeconds + 30))
    # The app connects as soon as its script says so, which can be before a
    # helper that is still starting has bound its port.
    Start-Sleep -Milliseconds 700
}

try {
    $proc = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path $Exe) -PassThru
    if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-Process -Id $proc.Id -Force
        Write-Host "!!! Timed out after ${TimeoutSeconds}s" -ForegroundColor Red
    }
}
finally {
    Remove-Item Env:OFS_PREF_PATH, Env:OFS_UITEST_SCRIPT, Env:OFS_UITEST_OUT, Env:OFS_UITEST_MEDIA -ErrorAction SilentlyContinue
    if ($helper -and -not $helper.HasExited) {
        Stop-Process -Id $helper.Id -Force -ErrorAction SilentlyContinue
    }
}

$log = Join-Path $Out 'driver.log'
if (-not (Test-Path $log)) {
    Write-Host '!!! No driver.log; the app did not run the script' -ForegroundColor Red
    exit 1
}
Get-Content $log | ForEach-Object {
    if ($_ -match 'FAIL') { Write-Host $_ -ForegroundColor Red }
    elseif ($_ -match 'PASS') { Write-Host $_ -ForegroundColor Green }
    else { Write-Host $_ }
}
$text = Get-Content $log -Raw
$finished = $text -match 'done:'
# Case sensitive and anchored: -match ignores case, and "0 failed" on the
# summary line would otherwise count as a failure.
$anyFailed = $text -cmatch ': FAIL '
if (-not $finished) {
    Write-Host '!!! The script did not run to the end' -ForegroundColor Red
    exit 1
}
if ($anyFailed) {
    Write-Host '!!! Some checks failed' -ForegroundColor Red
    exit 1
}
Write-Host "==> Passed. Results in $Out" -ForegroundColor Cyan
exit 0
