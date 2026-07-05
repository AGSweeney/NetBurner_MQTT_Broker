#ifndef MQTT_BROKER_LIMITS_NANO54415_HPP
#define MQTT_BROKER_LIMITS_NANO54415_HPP

// NANO54415 production profile. Sized from Phase 0 RAM/CPU measurements —
// docs/feasibility-NANO54415.md (2026-07-05). Primary reference target for broker capacity.

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
    static constexpr int TxLogicalTotal    = 262144;
    static constexpr int PayloadBlockSize  = 512;
    static constexpr int PayloadBlockCount = 256;   // 128 KB payload pool
    static constexpr int MaxInternedTopics = 256;
    static constexpr int MaxInternedBytes  = 32768;
    static constexpr int TlsHandshakeSlots = 1;
    static constexpr int TlsPendingAccepts = 1;
    static constexpr int Pbkdf2SaltBytes   = 16;
    static constexpr int Pbkdf2KeyBytes    = 32;
    static constexpr int Pbkdf2Iterations  = 400;   // provisional; HiRes re-measure pending
    static constexpr int TxQueueDepthPerClient = 64;
    static constexpr int PayloadSlotCount  = TxQueueDepthPerClient + MaxTcpClients;
    static constexpr int MaxRetainedEntries  = 128;
    static constexpr int KeepAliveMultiplierTenths = 15;
    static constexpr int MaxKeepAliveSec = 30;
    static constexpr int ConnectHandshakeTimeoutSec = 15;
};

#endif
