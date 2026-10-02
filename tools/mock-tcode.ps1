<#
.SYNOPSIS
    A stand-in for a TCode machine, to check what OFS sends without one.

.DESCRIPTION
    Listens for TCode over UDP or TCP and writes every line it receives to a
    log, so a test can see the commands, their axes, and their intervals.

    A real OSR is the thing to try before trusting any of this with hardware;
    this only shows that OFS says the right words.

.EXAMPLE
    tools\mock-tcode.ps1 -Log build\uitest\tcode.log
    Listens on udp://127.0.0.1:8000 until the timeout.
#>
[CmdletBinding()]
param(
    [ValidateSet('udp', 'tcp')] [string] $Protocol = 'udp',
    [int] $Port = 8000,
    [string] $Log = 'mock-tcode.log',
    [int] $TimeoutSeconds = 120
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$logDir = Split-Path -Parent $Log
if ($logDir -and -not (Test-Path $logDir)) { New-Item -ItemType Directory -Force $logDir | Out-Null }
Set-Content -Path $Log -Value "listening on ${Protocol}://127.0.0.1:$Port" -Encoding utf8

function Write-Log([string] $line) {
    Add-Content -Path $Log -Value $line -Encoding utf8
}

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)

if ($Protocol -eq 'udp') {
    $client = [Net.Sockets.UdpClient]::new($Port)
    try {
        $endpoint = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
        while ((Get-Date) -lt $deadline) {
            if ($client.Available -le 0) {
                Start-Sleep -Milliseconds 10
                continue
            }
            $bytes = $client.Receive([ref]$endpoint)
            $text = [Text.Encoding]::UTF8.GetString($bytes).TrimEnd("`r", "`n")
            foreach ($line in ($text -split "`n")) {
                if ($line) { Write-Log "recv $line" }
            }
        }
    }
    finally { $client.Close() }
}
else {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
    $listener.Start()
    try {
        while ((Get-Date) -lt $deadline -and -not $listener.Pending()) {
            Start-Sleep -Milliseconds 20
        }
        if (-not $listener.Pending()) {
            Write-Log 'no connection'
            return
        }
        $client = $listener.AcceptTcpClient()
        Write-Log 'connected'
        $stream = $client.GetStream()
        $reader = [IO.StreamReader]::new($stream)
        while ((Get-Date) -lt $deadline -and $client.Connected) {
            if (-not $stream.DataAvailable) {
                Start-Sleep -Milliseconds 10
                continue
            }
            $line = $reader.ReadLine()
            if ($null -eq $line) { break }
            if ($line) { Write-Log "recv $line" }
        }
        $client.Close()
    }
    finally { $listener.Stop() }
}
Write-Log 'closed'
