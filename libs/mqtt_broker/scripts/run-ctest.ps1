# Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
# SPDX-License-Identifier: MIT

# Host test runner: no Windows assert popups, stale-process cleanup, full stderr capture.
$ErrorActionPreference = 'Stop'
$BuildDir = Join-Path $PSScriptRoot '..' 'build'
$DebugDir = Join-Path $BuildDir 'Debug'

Get-Process test_broker,test_tx_queue,fuzz_parser,test_wire,test_varint,test_message_pool,
    test_topic_intern,test_topic_trie,test_qos_downgrade,test_properties,test_parser `
    -ErrorAction SilentlyContinue | Stop-Process -Force

$env:CTEST_OUTPUT_ON_FAILURE = '1'
Push-Location $BuildDir
try {
    & ctest -C Debug --output-on-failure 2>&1 | ForEach-Object { Write-Output $_ }
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}
exit 0