// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef MQTT_BROKER_LIMITS_MODM7AE70_HPP
#define MQTT_BROKER_LIMITS_MODM7AE70_HPP

// MODM7AE70 production profile. Tighter client and payload pool than NANO due to
// smaller RAM budget — docs/feasibility-MODM7AE70.md (2026-07-05).

struct BrokerLimits {
    static constexpr int MaxTcpClients     = 16;
    static constexpr int MaxTlsClients     = 2;
    static constexpr int MaxSubsPerClient  = 16;
    static constexpr int MaxSubsTotal      = 128;
    static constexpr int MaxPacketBytes    = 16384;
    static constexpr int MaxTopicBytes     = 256;
    static constexpr int MaxPayloadBytes   = 8192;
    static constexpr int QosInflightPerClient = 8;
    static constexpr int OfflineQueuePerClient = 16;
    static constexpr int RetainedBytesTotal = 32768;
    static constexpr int ParserReadChunk   = 512;
    static constexpr int TxLogicalPerClient = 32768;
    static constexpr int TxLogicalTotal    = 196608;
    static constexpr int PayloadBlockSize  = 512;
    static constexpr int PayloadBlockCount = 144;   // 72 KB payload pool
    static constexpr int MaxInternedTopics = 128;
    static constexpr int MaxInternedBytes  = 16384;
    static constexpr int TlsHandshakeSlots = 1;
    static constexpr int TlsPendingAccepts = 1;
    static constexpr int Pbkdf2SaltBytes   = 16;
    static constexpr int Pbkdf2KeyBytes    = 32;
    static constexpr int Pbkdf2Iterations  = 1000;
    static constexpr int TxQueueDepthPerClient = 32;
    static constexpr int PayloadSlotCount  = TxQueueDepthPerClient + MaxTcpClients;
    static constexpr int MaxRetainedEntries  = 64;
    static constexpr int KeepAliveMultiplierTenths = 15;
    static constexpr int MaxKeepAliveSec = 30;
    static constexpr int ConnectHandshakeTimeoutSec = 15;
};

#endif
