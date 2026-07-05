// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef MQTT_BROKER_PENDING_DELIVERY_HPP
#define MQTT_BROKER_PENDING_DELIVERY_HPP

// Outbound PUBLISH work item shared by OfflineQueue and TxQueue. Pool and topic
// handles are ref-counted by the broker around enqueue/pop — this struct only
// carries the delivery metadata and incremental encode progress.

#include "mqtt_broker/topic_intern.hpp"
#include "mqtt_broker/types.hpp"

#include <cstdint>

namespace mqtt_broker {

struct PendingDelivery {
    MessageHandle message;
    TopicHandle topic;
    uint16_t packet_id;           // 0 for QoS 0
    uint32_t encoded_offset;      // Bytes already sent of encoded_total_size
    uint32_t encoded_total_size;  // Full on-wire PUBLISH estimate at enqueue
    uint8_t delivery_qos;
    bool retain_flag;
    bool dup_flag;  // Set on QoS 1/2 retransmit (MQTT-5.0 §3.3.1.1)
};

}  // namespace mqtt_broker

#endif
