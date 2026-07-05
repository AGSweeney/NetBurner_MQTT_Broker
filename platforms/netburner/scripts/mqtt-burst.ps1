# Publish a burst of MQTT 5 QoS0 test messages for client verification.
param(
    [string]$BrokerHost = '172.16.82.55',
    [int]$Port = 1883,
    [int]$Count = 20,
    [int]$DelayMs = 150,
    [switch]$Json
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'mqtt-lib.ps1')

function Build-JsonPayload([hashtable]$Message) {
    $ts = (Get-Date).ToString('o')
    switch ($Message.Kind) {
        'sensor-temp' {
            return (@{
                device = 'nano54415'
                metric = 'temp'
                unit   = 'C'
                value  = 22.1
                ts     = $ts
            } | ConvertTo-Json -Compress)
        }
        'sensor-humidity' {
            return (@{
                device = 'nano54415'
                metric = 'humidity'
                unit   = '%'
                value  = 45
                ts     = $ts
            } | ConvertTo-Json -Compress)
        }
        'sensor-pressure' {
            return (@{
                device = 'nano54415'
                metric = 'pressure'
                unit   = 'hPa'
                value  = 1013
                ts     = $ts
            } | ConvertTo-Json -Compress)
        }
        'hello' {
            return (@{
                event   = 'hello'
                message = 'Hello from burst publisher'
                ts      = $ts
            } | ConvertTo-Json -Compress)
        }
        'burst' {
            return (@{
                seq     = $Message.Seq
                total   = $Message.Total
                event   = 'burst'
                message = "Burst message $($Message.Seq) of $($Message.Total)"
                ts      = $ts
            } | ConvertTo-Json -Compress)
        }
        default {
            throw "Unknown JSON message kind: $($Message.Kind)"
        }
    }
}

$ClientId = 'burst-publisher-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$tcp = New-MqttTcp $BrokerHost $Port
$stream = $tcp.GetStream()
Write-Host "Connected to ${BrokerHost}:${Port} as $ClientId"

Send-MqttPacket $stream (Build-Connect $ClientId)
$connack = Read-MqttPacket $stream 5000
if ($connack.Length -lt 2 -or $connack[0] -ne 0x20) {
    Write-Error "CONNACK failed: got $([BitConverter]::ToString($connack))"
}
Write-Host "CONNACK ok"

$messages = @()
if ($Json) {
    $messages += @{ Topic = 'sensors/temp'; Kind = 'sensor-temp' }
    $messages += @{ Topic = 'sensors/humidity'; Kind = 'sensor-humidity' }
    $messages += @{ Topic = 'sensors/pressure'; Kind = 'sensor-pressure' }
    $messages += @{ Topic = 'test/hello'; Kind = 'hello' }
    for ($i = 1; $i -le $Count; $i++) {
        $messages += @{
            Topic = "test/burst/$i"
            Kind  = 'burst'
            Seq   = $i
            Total = $Count
        }
    }
} else {
    $messages = @(
        @{ Topic = 'sensors/temp'; Payload = '22.1 C - warmup' },
        @{ Topic = 'sensors/humidity'; Payload = '45% RH' },
        @{ Topic = 'sensors/pressure'; Payload = '1013 hPa' },
        @{ Topic = 'test/hello'; Payload = 'Hello from burst publisher' }
    )
    for ($i = 1; $i -le $Count; $i++) {
        $messages += @{
            Topic = "test/burst/$i"
            Seq   = $i
            Total = $Count
        }
    }
}

$n = 0
foreach ($m in $messages) {
    if ($Json) {
        $payload = Build-JsonPayload $m
    } elseif ($m.Seq) {
        $ts = Get-Date -Format 'HH:mm:ss.fff'
        $payload = "Burst message $($m.Seq) of $($m.Total) at $ts"
    } else {
        $payload = $m.Payload
    }

    Send-MqttPacket $stream (Build-Publish $m.Topic $payload)
    $n++
    Write-Host "[$n/$($messages.Count)] $($m.Topic) -> $payload"
    if ($DelayMs -gt 0) {
        Start-Sleep -Milliseconds $DelayMs
    }
}

Start-Sleep -Milliseconds 300
Send-MqttPacket $stream (Build-Disconnect)
Start-Sleep -Milliseconds 100
$tcp.Close()
Write-Host "DISCONNECT sent, TCP closed"
Write-Host "BURST_OK sent $n messages"
