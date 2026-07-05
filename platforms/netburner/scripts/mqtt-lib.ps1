# Shared MQTT 5 packet helpers for device smoke/qual scripts.

function Send-MqttPacket($Stream, [byte[]]$Packet) {
    $Stream.Write($Packet, 0, $Packet.Length)
    $Stream.Flush()
}

function Read-MqttPacket($Stream, [int]$TimeoutMs = 5000) {
    $deadline = [datetime]::UtcNow.AddMilliseconds($TimeoutMs)
    $buf = New-Object System.Collections.Generic.List[byte]
    while ([datetime]::UtcNow -lt $deadline) {
        if ($Stream.DataAvailable) {
            $chunk = New-Object byte[] 4096
            $n = $Stream.Read($chunk, 0, $chunk.Length)
            if ($n -le 0) { break }
            for ($i = 0; $i -lt $n; $i++) { [void]$buf.Add($chunk[$i]) }
            if ($buf.Count -ge 2) {
                $remaining = 0
                $mult = 1
                $idx = 1
                do {
                    if ($idx -ge $buf.Count) { break }
                    $b = $buf[$idx]
                    $remaining += ($b -band 0x7F) * $mult
                    $mult *= 128
                    $idx++
                } while (($b -band 0x80) -ne 0 -and $idx -lt 5)
                $need = $idx + $remaining
                if ($buf.Count -ge $need) { break }
            }
        } else {
            Start-Sleep -Milliseconds 20
        }
    }
    return $buf.ToArray()
}

function Build-Connect([string]$Id) {
    $body = New-Object System.Collections.Generic.List[byte]
    $body.AddRange([byte[]](0x00, 0x04, 0x4D, 0x51, 0x54, 0x54, 0x05, 0x02, 0x00, 0x3C, 0x00))
    $idBytes = [System.Text.Encoding]::UTF8.GetBytes($Id)
    $body.Add([byte](($idBytes.Length -shr 8) -band 0xFF))
    $body.Add([byte]($idBytes.Length -band 0xFF))
    $body.AddRange($idBytes)
    $remaining = $body.Count
    $rl = New-Object System.Collections.Generic.List[byte]
    do {
        $digit = $remaining -band 0x7F
        $remaining = $remaining -shr 7
        if ($remaining -gt 0) { $digit = $digit -bor 0x80 }
        $rl.Add([byte]$digit)
    } while ($remaining -gt 0)
    $pkt = New-Object System.Collections.Generic.List[byte]
    $pkt.Add(0x10)
    $pkt.AddRange($rl)
    $pkt.AddRange($body)
    return $pkt.ToArray()
}

function Build-Subscribe([uint16]$PacketId, [string]$Filter) {
    $body = New-Object System.Collections.Generic.List[byte]
    $body.Add([byte](($PacketId -shr 8) -band 0xFF))
    $body.Add([byte]($PacketId -band 0xFF))
    $body.Add(0x00)
    $fBytes = [System.Text.Encoding]::UTF8.GetBytes($Filter)
    $body.Add([byte](($fBytes.Length -shr 8) -band 0xFF))
    $body.Add([byte]($fBytes.Length -band 0xFF))
    $body.AddRange($fBytes)
    $body.Add(0x00)
    $remaining = $body.Count
    $rl = New-Object System.Collections.Generic.List[byte]
    do {
        $digit = $remaining -band 0x7F
        $remaining = $remaining -shr 7
        if ($remaining -gt 0) { $digit = $digit -bor 0x80 }
        $rl.Add([byte]$digit)
    } while ($remaining -gt 0)
    $pkt = New-Object System.Collections.Generic.List[byte]
    $pkt.Add(0x82)
    $pkt.AddRange($rl)
    $pkt.AddRange($body)
    return $pkt.ToArray()
}

function Build-Disconnect() {
    return [byte[]](0xE0, 0x00)
}

function Build-Publish([string]$Topic, [string]$Payload) {
    $body = New-Object System.Collections.Generic.List[byte]
    $tBytes = [System.Text.Encoding]::UTF8.GetBytes($Topic)
    $body.Add([byte](($tBytes.Length -shr 8) -band 0xFF))
    $body.Add([byte]($tBytes.Length -band 0xFF))
    $body.AddRange($tBytes)
    $body.Add(0x00)
    $pBytes = [System.Text.Encoding]::UTF8.GetBytes($Payload)
    $body.AddRange($pBytes)
    $remaining = $body.Count
    $rl = New-Object System.Collections.Generic.List[byte]
    do {
        $digit = $remaining -band 0x7F
        $remaining = $remaining -shr 7
        if ($remaining -gt 0) { $digit = $digit -bor 0x80 }
        $rl.Add([byte]$digit)
    } while ($remaining -gt 0)
    $pkt = New-Object System.Collections.Generic.List[byte]
    $pkt.Add(0x30)
    $pkt.AddRange($rl)
    $pkt.AddRange($body)
    return $pkt.ToArray()
}

function New-MqttTcp($BrokerHost, $Port) {
    $tcp = New-Object System.Net.Sockets.TcpClient
    $tcp.ReceiveTimeout = 8000
    $tcp.SendTimeout = 8000
    $tcp.Connect($BrokerHost, $Port)
    return $tcp
}