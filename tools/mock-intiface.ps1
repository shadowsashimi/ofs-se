<#
.SYNOPSIS
    A stand-in for Intiface Central, to check the Devices panel without one.

.DESCRIPTION
    Speaks just enough Buttplug to answer OFS: it reports a server, offers one
    device that can be told a position, and writes every message it receives to
    a log, so a test can see what OFS sent and when.

    Real Intiface is the thing to test against before trusting any of this with
    hardware; this only shows that OFS says the right words in the right order.

.EXAMPLE
    tools\mock-intiface.ps1 -Log build\uitest\intiface.log
    Serves on ws://127.0.0.1:12345 until the first connection closes.
#>
[CmdletBinding()]
param(
    [int] $Port = 12345,
    [string] $Log = 'mock-intiface.log',
    [int] $TimeoutSeconds = 120
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$logDir = Split-Path -Parent $Log
if ($logDir -and -not (Test-Path $logDir)) { New-Item -ItemType Directory -Force $logDir | Out-Null }
Set-Content -Path $Log -Value "listening on ws://127.0.0.1:$Port/" -Encoding utf8

$listener = [System.Net.HttpListener]::new()
$listener.Prefixes.Add("http://127.0.0.1:$Port/")
$listener.Start()

function Write-Log([string] $line) {
    Add-Content -Path $Log -Value $line -Encoding utf8
}

try {
    $contextTask = $listener.GetContextAsync()
    if (-not $contextTask.Wait($TimeoutSeconds * 1000)) {
        Write-Log 'no connection'
        return
    }
    $context = $contextTask.Result
    if (-not $context.Request.IsWebSocketRequest) {
        $context.Response.StatusCode = 400
        $context.Response.Close()
        Write-Log 'not a websocket request'
        return
    }

    # [NullString]::Value, because PowerShell turns a plain $null argument into
    # an empty string and AcceptWebSocketAsync rejects that as a sub-protocol.
    $socket = $context.AcceptWebSocketAsync([NullString]::Value).GetAwaiter().GetResult().WebSocket
    Write-Log 'connected'
    $buffer = [byte[]]::new(8192)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)

    while ($socket.State -eq 'Open' -and (Get-Date) -lt $deadline) {
        $segment = [ArraySegment[byte]]::new($buffer)
        # The app closing mid-receive is the normal way this ends, and throws
        # here rather than returning a close message.
        try {
            $receive = $socket.ReceiveAsync($segment, [Threading.CancellationToken]::None)
            if (-not $receive.Wait(2000)) { continue }
            $result = $receive.Result
        }
        catch { break }
        if ($result.MessageType -eq 'Close') { break }

        $text = [Text.Encoding]::UTF8.GetString($buffer, 0, $result.Count)
        Write-Log "recv $text"

        $replies = @()
        foreach ($message in ($text | ConvertFrom-Json)) {
            foreach ($property in $message.PSObject.Properties) {
                $id = 1
                if ($property.Value.PSObject.Properties.Name -contains 'Id') { $id = $property.Value.Id }
                switch ($property.Name) {
                    'RequestServerInfo' {
                        $replies += "{""ServerInfo"":{""Id"":$id,""ServerName"":""Mock Intiface"",""MessageVersion"":3,""MaxPingTime"":0}}"
                    }
                    'RequestDeviceList' {
                        $replies += "{""DeviceList"":{""Id"":$id,""Devices"":[{""DeviceName"":""Mock stroker"",""DeviceIndex"":0,""DeviceMessages"":{""LinearCmd"":[{""StepCount"":100,""FeatureDescriptor"":""stroke"",""ActuatorType"":""Position""}],""StopDeviceCmd"":{}}}]}}"
                    }
                    default {
                        $replies += "{""Ok"":{""Id"":$id}}"
                    }
                }
            }
        }
        if ($replies.Count -gt 0) {
            $payload = '[' + ($replies -join ',') + ']'
            $bytes = [Text.Encoding]::UTF8.GetBytes($payload)
            $socket.SendAsync([ArraySegment[byte]]::new($bytes), 'Text', $true,
                [Threading.CancellationToken]::None).Wait(2000) | Out-Null
            Write-Log "sent $payload"
        }
    }
    Write-Log 'closed'
}
finally {
    $listener.Stop()
    $listener.Close()
}
