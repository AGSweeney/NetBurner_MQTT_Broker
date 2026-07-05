# Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
# SPDX-License-Identifier: MIT

# Minimal MQTT 5 QoS0 smoke test against nb-mqtt-broker on port 1883.
param(
    [string]$BrokerHost = '172.16.82.52',
    [int]$Port = 1883,
    [string]$ClientId = 'smoke'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'mqtt-lib.ps1')

$tcp = New-MqttTcp $BrokerHost $Port
$stream = $tcp.GetStream()

Write-Host "Connected to ${BrokerHost}:${Port}"

Send-MqttPacket $stream (Build-Connect $ClientId)
$connack = Read-MqttPacket $stream
if ($connack.Length -lt 2 -or $connack[0] -ne 0x20) {
    Write-Error "CONNACK failed: got $([BitConverter]::ToString($connack))"
}
Write-Host "CONNACK ok (reason=$($connack[3]))"

Send-MqttPacket $stream (Build-Subscribe 1 'sensors/#')
$suback = Read-MqttPacket $stream
if ($suback.Length -lt 2 -or $suback[0] -ne 0x90) {
    Write-Error "SUBACK failed: got $([BitConverter]::ToString($suback))"
}
Write-Host "SUBACK ok"

Send-MqttPacket $stream (Build-Publish 'sensors/temp' '42.5')
$publish = Read-MqttPacket $stream 8000
if ($publish.Length -lt 4 -or $publish[0] -ne 0x30) {
    Write-Error "PUBLISH delivery failed: got $([BitConverter]::ToString($publish))"
}
$payload = [System.Text.Encoding]::UTF8.GetString($publish[($publish.Length - 4)..($publish.Length - 1)])
Write-Host "Received publish payload tail: $payload"

$tcp.Close()
Write-Host "SMOKE_OK"