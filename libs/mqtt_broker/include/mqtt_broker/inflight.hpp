// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// Per-session QoS 1/2 inflight tracking: outbound PUBLISH/PUBREL exchange state
// and inbound QoS 2 dedup while awaiting PUBREL. Fixed-size tables sized by
// BrokerLimits::QosInflightPerClient (MQTT-5.0 §4.4, §4.3.3).

#ifndef MQTT_BROKER_INFLIGHT_HPP
#define MQTT_BROKER_INFLIGHT_HPP

#include "mqtt_broker/broker_limits.hpp"
#include "mqtt_broker/message_pool.hpp"
#include "mqtt_broker/topic_intern.hpp"
#include "mqtt_broker/types.hpp"

#include <cstddef>
#include <cstdint>

namespace mqtt_broker {

// Outbound QoS 2 handshake phase after the broker sends PUBLISH to the client.
enum class OutboundQoS2Phase : uint8_t {
    AwaitingPubrec = 1,   // PUBLISH sent; waiting for client PUBREC
    AwaitingPubcomp = 2,  // PUBREL sent; waiting for client PUBCOMP (§4.3.3)
};

// One outbound QoS 1/2 delivery held until PUBACK (QoS 1) or PUBCOMP (QoS 2).
// Holds ref-counted message/topic handles; released on ack or session end.
struct OutboundInflightEntry {
    bool in_use;
    uint16_t packet_id;              // Non-zero; unique within the session table
    MessageHandle message;
    TopicHandle topic;
    uint8_t delivery_qos;            // Effective QoS sent on the wire (1 or 2)
    bool retain_flag;
    OutboundQoS2Phase qos2_phase;     // Meaningful only when delivery_qos == 2
};

// Fixed-capacity outbound inflight table for a single session. Packet IDs are
// allocated locally; capacity is bounded by effective_receive_maximum().
class SessionOutboundInflight {
public:
    SessionOutboundInflight();

    // Release all entries and topic/message refs. Safe with null pool/topics.
    void clear(MessagePool *pool, TopicInternPool *topics);
    size_t count() const;
    bool at_capacity() const;

    // Returns the next unused packet identifier, or 0 if the table is full.
    // IDs wrap 1..65535; 0 is never assigned (MQTT-5.0 §2.2.1).
    uint16_t next_packet_id();

    // Reset the allocation cursor; called when a session ends (not on disconnect).
    void reset_packet_ids();

    // Takes ownership of msg/topic refs on success. packet_id must be non-zero
    // and unused. qos2_phase applies only when delivery_qos >= 2.
    bool insert(uint16_t packet_id, MessageHandle msg, TopicHandle topic, uint8_t delivery_qos,
                bool retain_flag, OutboundQoS2Phase qos2_phase);
    OutboundInflightEntry *find(uint16_t packet_id);
    bool remove(uint16_t packet_id, MessagePool *pool, TopicInternPool *topics);

    static constexpr size_t capacity() { return BrokerLimits::QosInflightPerClient; }
    OutboundInflightEntry *entry_at(size_t index)
    {
        return index < capacity() ? &entries_[index] : nullptr;
    }

private:
    OutboundInflightEntry entries_[BrokerLimits::QosInflightPerClient];
    uint16_t next_id_;  // Monotonic cursor for next_packet_id(); wraps at 65535
};

// Staged inbound QoS 2 PUBLISH between first receipt and PUBREL (§4.3.3).
// Topic is copied because the client may disconnect before PUBREL arrives.
struct InboundQoS2Entry {
    bool in_use;
    uint16_t packet_id;
    char topic[256];
    MessageHandle payload;
    size_t payload_len;
    bool retain;
    uint8_t qos;
    bool dup;
    bool pubrec_sent;  // PUBREC already sent for this packet_id
};

// Fixed-capacity inbound QoS 2 dedup table. Duplicate PUBLISH with the same
// packet_id returns the existing entry instead of storing twice.
class SessionInboundQoS2 {
public:
    SessionInboundQoS2();

    void clear(MessagePool *pool);
    bool at_capacity() const;

    InboundQoS2Entry *find(uint16_t packet_id);
    InboundQoS2Entry *insert(uint16_t packet_id, const char *topic, MessageHandle payload,
                             size_t payload_len, bool retain, uint8_t qos, bool dup);
    bool remove(uint16_t packet_id, MessagePool *pool);

private:
    InboundQoS2Entry entries_[BrokerLimits::QosInflightPerClient];
};

// Clamp the client's Receive Maximum CONNECT property to what the broker can
// honor: min(client, CONNACK Receive Maximum, table capacity). Zero client
// value means "use CONNACK default" (MQTT-5.0 §3.2.2.3.3).
uint16_t effective_receive_maximum(uint16_t client_receive_maximum);

}  // namespace mqtt_broker

#endif
