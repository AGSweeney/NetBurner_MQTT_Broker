// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef MQTT_BROKER_TX_QUEUE_HPP
#define MQTT_BROKER_TX_QUEUE_HPP

// Per-connection outbound PUBLISH queue drained by Broker::drain_tx(). Tracks
// logical wire bytes (sum of encoded_total_size) against logical_limit so large
// payloads can stream in windowed chunks without holding a full packet buffer.
// Admission rejects when depth or logical budget is exceeded.

#include "mqtt_broker/pending_delivery.hpp"

#include <cstddef>
#include <cstdint>

namespace mqtt_broker {

struct TxQueueConfig {
    size_t depth;            // Max in-flight deliveries per transport
    uint32_t logical_limit;  // Sum of encoded_total_size cap for this connection
};

// Ring buffer; front entry may be partially sent (encoded_offset < encoded_total_size).
class TxQueue {
public:
    explicit TxQueue(const TxQueueConfig &cfg);

    // encoded_size is the wire estimate stored on the entry for quota accounting.
    bool enqueue(const PendingDelivery &delivery, uint32_t encoded_size);
    bool peek(PendingDelivery *out) const;
    // Decrements logical_bytes_ by the popped entry's encoded_total_size.
    bool pop(uint32_t *released_logical);
    // Advances partial-send progress on the front entry after a write() slice.
    bool advance_offset(uint32_t delta);
    uint32_t front_offset() const;

    uint32_t logical_bytes() const { return logical_bytes_; }  // Current queued wire bytes
    uint32_t logical_high_water() const { return logical_high_water_; }  // Peak since construct
    size_t count() const { return count_; }
    bool full() const { return count_ >= depth_; }

private:
    PendingDelivery *entries_;
    size_t depth_;
    size_t head_;
    size_t tail_;
    size_t count_;
    uint32_t logical_bytes_;       // Running sum of enqueued encoded sizes
    uint32_t logical_limit_;       // From TxQueueConfig
    uint32_t logical_high_water_;  // Diagnostic peak for metrics/debug
};

}  // namespace mqtt_broker

#endif
