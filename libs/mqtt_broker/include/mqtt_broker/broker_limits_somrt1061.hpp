// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef MQTT_BROKER_LIMITS_SOMRT1061_HPP
#define MQTT_BROKER_LIMITS_SOMRT1061_HPP

// SOMRT1061 production profile. Sized from Phase 0 measurements on target hardware
// at 172.16.82.8 (2026-07-05). Payload pool aligned with NANO54415.

struct BrokerLimits {
    static constexpr int MaxTcpClients     = 32;
    static constexpr int MaxTlsClients     = 2;
    static constexpr int MaxSubsPerClient  = 16;
    static constexpr int MaxSubsTotal      = 128;
    static constexpr int MaxPacketBytes    = 16384;
    static constexpr int MaxTopicBytes     = 256;
    static constexpr int MaxPayloadBytes   = 8192;
    static constexpr int QosInflightPerClient = 8;
    static constexpr int OfflineQueuePerClient = 16;
    static constexpr int RetainedBytesTotal = 65536;
    static constexpr int ParserReadChunk   = 1024;
    static constexpr int TxLogicalPerClient = 131072;
    static constexpr int TxLogicalTotal    = 262144;
    static constexpr int PayloadBlockSize  = 512;
    static constexpr int PayloadBlockCount = 384;   // 192 KB payload pool
    static constexpr int MaxInternedTopics = 256;
    static constexpr int MaxInternedBytes  = 32768;
    static constexpr int TlsHandshakeSlots = 1;
    static constexpr int TlsPendingAccepts = 1;
    static constexpr int Pbkdf2SaltBytes   = 16;
    static constexpr int Pbkdf2KeyBytes    = 32;
    static constexpr int Pbkdf2Iterations  = 9000;
    static constexpr int TxQueueDepthPerClient = 96;
    static constexpr int PayloadSlotCount  = TxQueueDepthPerClient + MaxTcpClients;
    static constexpr int MaxRetainedEntries  = 96;
    static constexpr int KeepAliveMultiplierTenths = 15;
    static constexpr int MaxKeepAliveSec = 30;
    static constexpr int ConnectHandshakeTimeoutSec = 15;
};

#endif
