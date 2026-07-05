// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef MQTT_BROKER_SESSION_HPP
#define MQTT_BROKER_SESSION_HPP

// Session identity and per-client state. A session slot survives disconnect when
// Session Expiry Interval > 0; subscriptions, inflight QoS, and offline queue
// are keyed by slot + generation to detect stale handles after reuse.

#include "mqtt_broker/types.hpp"

#include <cstdint>

namespace mqtt_broker {

// Generation-checked handle: valid only while self.generation matches the slot.
struct SessionHandle {
    uint16_t slot;
    uint16_t generation;
};

static const SessionHandle NULL_SESSION = {0xFFFF, 0xFFFF};

inline bool session_handle_valid(SessionHandle h)
{
    return h.slot != 0xFFFF || h.generation != 0xFFFF;
}

enum class SessionState : uint8_t {
    Empty = 0,      // Slot unused or fully torn down
    Connected,      // Active transport attached
    Offline         // Disconnected but session data retained (MQTT-5.0 §3.1.2.5)
};

struct WillMessage {
    bool valid;
    char topic[256];
    MessageHandle payload;
    size_t payload_len;
    uint8_t qos;
    bool retain;
};

struct SessionRecord {
    SessionHandle self;
    SessionState state;
    uint16_t transport_id;              // BrokerLimits transport slot while Connected
    char client_id[64];
    uint16_t keep_alive_sec;
    uint32_t client_max_packet_size;
    uint16_t client_receive_maximum;    // 0 => use broker CONNACK Receive Maximum
    uint32_t last_control_tick;         // Updated on any control packet (keep-alive)
    bool clean_start;
    uint32_t session_expiry_interval;   // Seconds; 0 ends session on disconnect
    uint32_t session_expiry_deadline_tick;
    uint32_t will_delay_interval;
    uint32_t will_fire_deadline_tick;   // Absolute tick for delayed will while offline
    WillMessage will;
    bool suppress_will;                 // Set on graceful DISCONNECT reason 0x00
    uint32_t assign_serial;             // Server-assigned Client ID suffix (nb-{slot}-{n})
    bool has_subscription_state;        // True once SUBSCRIBE has been accepted
};

static const uint32_t SESSION_TICK_NONE = 0xFFFFFFFFu;  // No deadline armed

}  // namespace mqtt_broker

#endif