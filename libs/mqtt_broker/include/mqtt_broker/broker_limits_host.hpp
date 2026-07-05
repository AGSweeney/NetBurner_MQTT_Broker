// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef MQTT_BROKER_LIMITS_HOST_HPP
#define MQTT_BROKER_LIMITS_HOST_HPP

// Host / LE unit-test profile. Mirrors NANO54415 sizing so acceptance tests exercise
// the same limits as the primary hardware target without needing the board.

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
    static constexpr int ParserReadChunk   = 512;
    static constexpr int TxLogicalPerClient = 32768;
    static constexpr int TxLogicalTotal    = 131072;
    static constexpr int PayloadBlockSize  = 512;
    static constexpr int PayloadBlockCount = 96;
    static constexpr int MaxInternedTopics = 128;
    static constexpr int MaxInternedBytes  = 16384;
    static constexpr int TlsHandshakeSlots = 1;
    static constexpr int TlsPendingAccepts = 1;
    static constexpr int Pbkdf2SaltBytes   = 16;
    static constexpr int Pbkdf2KeyBytes    = 32;
    static constexpr int Pbkdf2Iterations  = 400;
    static constexpr int TxQueueDepthPerClient = 64;
    static constexpr int PayloadSlotCount  = TxQueueDepthPerClient + MaxTcpClients;
    static constexpr int MaxRetainedEntries  = 64;
    static constexpr int KeepAliveMultiplierTenths = 15;
    static constexpr int MaxKeepAliveSec = 30;
    static constexpr int ConnectHandshakeTimeoutSec = 15;
};

#endif
