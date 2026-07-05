# Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
# SPDX-License-Identifier: MIT

# Phase 2 abbreviated device qualification for nb-mqtt-broker.
param(
    [string]$BrokerHost = '172.16.82.52',
    [int]$Port = 1883
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'mqtt-lib.ps1')

# 1) Fragmented CONNECT (one byte per write)
Write-Host "TEST fragmented_connect"
$pkt = Build-Connect 'frag'
$tcp = New-MqttTcp $BrokerHost $Port
$stream = $tcp.GetStream()
foreach ($b in $pkt) {
    Send-MqttPacket $stream ([byte[]]$b)
    Start-Sleep -Milliseconds 5
}
$ack = Read-MqttPacket $stream
if ($ack.Length -lt 2 -or $ack[0] -ne 0x20) {
    Write-Error "fragmented CONNECT failed: $([BitConverter]::ToString($ack))"
}
$tcp.Close()
Write-Host "  PASS"

# 2) Invalid packet on one connection; fresh connection must still work
Write-Host "TEST invalid_packet_recovery"
$tcp = New-MqttTcp $BrokerHost $Port
$stream = $tcp.GetStream()
Send-MqttPacket $stream ([byte[]](0xFF, 0x00))
Start-Sleep -Milliseconds 300
$tcp.Close()

$tcp2 = New-MqttTcp $BrokerHost $Port
$stream2 = $tcp2.GetStream()
Send-MqttPacket $stream2 (Build-Connect 'recover')
$ack = Read-MqttPacket $stream2 5000
if ($ack.Length -lt 2 -or $ack[0] -ne 0x20) {
    Write-Error "recovery CONNECT failed after invalid packet"
}
$tcp2.Close()
Write-Host "  PASS"

# 3) Eight TCP clients total (7 subscribers + 1 publisher)
Write-Host "TEST eight_clients_routing"
$clients = @()
for ($i = 0; $i -lt 7; $i++) {
    $tcp = New-MqttTcp $BrokerHost $Port
    $stream = $tcp.GetStream()
    Send-MqttPacket $stream (Build-Connect "sub$i")
    $ack = Read-MqttPacket $stream
    if ($ack[0] -ne 0x20 -or ($ack.Length -ge 4 -and $ack[3] -ne 0x00)) {
        Write-Error "client sub$i CONNACK failed: $([BitConverter]::ToString($ack))"
    }
    Send-MqttPacket $stream (Build-Subscribe ([uint16]($i + 1)) 'qual/#')
    $sack = Read-MqttPacket $stream
    if ($sack[0] -ne 0x90) { Write-Error "client sub$i SUBACK failed" }
    $clients += [pscustomobject]@{ Tcp = $tcp; Stream = $stream }
}

$pub = New-MqttTcp $BrokerHost $Port
$ps = $pub.GetStream()
Send-MqttPacket $ps (Build-Connect 'publisher')
$pack = Read-MqttPacket $ps
if ($pack[0] -ne 0x20 -or ($pack.Length -ge 4 -and $pack[3] -ne 0x00)) {
    Write-Error "publisher CONNACK failed: $([BitConverter]::ToString($pack))"
}
Send-MqttPacket $ps (Build-Publish 'qual/ping' 'hello-8')
Start-Sleep -Milliseconds 400

$delivered = 0
foreach ($c in $clients) {
    $msg = Read-MqttPacket $c.Stream 2500
    if ($msg.Length -ge 4 -and $msg[0] -eq 0x30) { $delivered++ }
    $c.Tcp.Close()
}
$pub.Close()
if ($delivered -lt 6) {
    Write-Error "expected >=6 deliveries to subscribers, got $delivered"
}
Write-Host "  PASS delivered=$delivered/7"

Write-Host "QUAL_OK"