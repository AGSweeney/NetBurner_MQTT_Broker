// Per-session inflight table operations and Receive Maximum clamping.

#include "mqtt_broker/inflight.hpp"

#include "mqtt_broker/connack_caps.hpp"
#include "mqtt_broker/message_pool.hpp"

#include <algorithm>
#include <cstring>

namespace mqtt_broker {

SessionOutboundInflight::SessionOutboundInflight() : next_id_(1)
{
    for (int i = 0; i < BrokerLimits::QosInflightPerClient; ++i) {
        entries_[i] = {};
    }
}

void SessionOutboundInflight::clear(MessagePool *pool, TopicInternPool *topics)
{
    for (int i = 0; i < BrokerLimits::QosInflightPerClient; ++i) {
        if (!entries_[i].in_use) {
            continue;
        }
        if (pool != nullptr && pool->msg_valid(entries_[i].message)) {
            pool->msg_release(entries_[i].message);
        }
        if (topics != nullptr) {
            topics->release(entries_[i].topic);
        }
        entries_[i] = {};
    }
}

size_t SessionOutboundInflight::count() const
{
    size_t n = 0;
    for (int i = 0; i < BrokerLimits::QosInflightPerClient; ++i) {
        if (entries_[i].in_use) {
            n++;
        }
    }
    return n;
}

bool SessionOutboundInflight::at_capacity() const
{
    return count() >= static_cast<size_t>(BrokerLimits::QosInflightPerClient);
}

uint16_t SessionOutboundInflight::next_packet_id()
{
    for (int attempt = 0; attempt < 65535; ++attempt) {
        if (next_id_ == 0) {
            next_id_ = 1;  // Skip reserved packet identifier 0 (§2.2.1)
        }
        uint16_t id = next_id_++;
        if (find(id) == nullptr) {
            return id;
        }
    }
    return 0;  // All 65535 IDs in use — caller must not send QoS 1/2
}

void SessionOutboundInflight::reset_packet_ids()
{
    next_id_ = 1;
}

bool SessionOutboundInflight::insert(uint16_t packet_id, MessageHandle msg, TopicHandle topic,
                                     uint8_t delivery_qos, bool retain_flag,
                                     OutboundQoS2Phase qos2_phase)
{
    if (packet_id == 0 || find(packet_id) != nullptr || at_capacity()) {
        return false;
    }
    for (int i = 0; i < BrokerLimits::QosInflightPerClient; ++i) {
        if (!entries_[i].in_use) {
            entries_[i].in_use = true;
            entries_[i].packet_id = packet_id;
            entries_[i].message = msg;
            entries_[i].topic = topic;
            entries_[i].delivery_qos = delivery_qos;
            entries_[i].retain_flag = retain_flag;
            // QoS 1 never uses qos2_phase; default keeps struct deterministic.
            entries_[i].qos2_phase =
                delivery_qos >= 2 ? qos2_phase : OutboundQoS2Phase::AwaitingPubrec;
            return true;
        }
    }
    return false;
}

OutboundInflightEntry *SessionOutboundInflight::find(uint16_t packet_id)
{
    for (int i = 0; i < BrokerLimits::QosInflightPerClient; ++i) {
        if (entries_[i].in_use && entries_[i].packet_id == packet_id) {
            return &entries_[i];
        }
    }
    return nullptr;
}

bool SessionOutboundInflight::remove(uint16_t packet_id, MessagePool *pool,
                                     TopicInternPool *topics)
{
    OutboundInflightEntry *e = find(packet_id);
    if (e == nullptr) {
        return false;
    }
    if (pool != nullptr && pool->msg_valid(e->message)) {
        pool->msg_release(e->message);
    }
    if (topics != nullptr) {
        topics->release(e->topic);
    }
    *e = {};
    return true;
}

SessionInboundQoS2::SessionInboundQoS2()
{
    for (int i = 0; i < BrokerLimits::QosInflightPerClient; ++i) {
        entries_[i] = {};
    }
}

void SessionInboundQoS2::clear(MessagePool *pool)
{
    for (int i = 0; i < BrokerLimits::QosInflightPerClient; ++i) {
        if (!entries_[i].in_use) {
            continue;
        }
        if (pool != nullptr && pool->msg_valid(entries_[i].payload)) {
            pool->msg_release(entries_[i].payload);
        }
        entries_[i] = {};
    }
}

bool SessionInboundQoS2::at_capacity() const
{
    size_t n = 0;
    for (int i = 0; i < BrokerLimits::QosInflightPerClient; ++i) {
        if (entries_[i].in_use) {
            n++;
        }
    }
    return n >= static_cast<size_t>(BrokerLimits::QosInflightPerClient);
}

InboundQoS2Entry *SessionInboundQoS2::find(uint16_t packet_id)
{
    for (int i = 0; i < BrokerLimits::QosInflightPerClient; ++i) {
        if (entries_[i].in_use && entries_[i].packet_id == packet_id) {
            return &entries_[i];
        }
    }
    return nullptr;
}

InboundQoS2Entry *SessionInboundQoS2::insert(uint16_t packet_id, const char *topic,
                                             MessageHandle payload, size_t payload_len,
                                             bool retain, uint8_t qos, bool dup)
{
    if (packet_id == 0 || topic == nullptr) {
        return nullptr;
    }
    InboundQoS2Entry *existing = find(packet_id);
    if (existing != nullptr) {
        // Duplicate QoS 2 PUBLISH: spec requires identical retransmission handling.
        return existing;
    }
    if (at_capacity()) {
        return nullptr;
    }
    for (int i = 0; i < BrokerLimits::QosInflightPerClient; ++i) {
        if (!entries_[i].in_use) {
            entries_[i].in_use = true;
            entries_[i].packet_id = packet_id;
            std::strncpy(entries_[i].topic, topic, sizeof(entries_[i].topic) - 1);
            entries_[i].topic[sizeof(entries_[i].topic) - 1] = '\0';
            entries_[i].payload = payload;
            entries_[i].payload_len = payload_len;
            entries_[i].retain = retain;
            entries_[i].qos = qos;
            entries_[i].dup = dup;
            entries_[i].pubrec_sent = false;
            return &entries_[i];
        }
    }
    return nullptr;
}

bool SessionInboundQoS2::remove(uint16_t packet_id, MessagePool *pool)
{
    InboundQoS2Entry *e = find(packet_id);
    if (e == nullptr) {
        return false;
    }
    if (pool != nullptr && pool->msg_valid(e->payload)) {
        pool->msg_release(e->payload);
    }
    *e = {};
    return true;
}

uint16_t effective_receive_maximum(uint16_t client_receive_maximum)
{
    uint16_t cap = static_cast<uint16_t>(BrokerLimits::QosInflightPerClient);
    uint16_t connack = static_cast<uint16_t>(ConnackCaps::ReceiveMaximum);
    uint16_t client = client_receive_maximum;
    if (client == 0) {
        client = connack;  // CONNECT omitted property → use CONNACK value
    }
    // Never advertise or accept more simultaneous QoS>0 sends than we can track.
    return std::min(cap, std::min(client, connack));
}

}  // namespace mqtt_broker
