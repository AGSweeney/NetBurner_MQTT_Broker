// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef MQTT_BROKER_OFFLINE_QUEUE_HPP
#define MQTT_BROKER_OFFLINE_QUEUE_HPP

// Per-session FIFO for QoS 1/2 PUBLISH deliveries while the client is offline
// (SessionState::Offline, MQTT-5.0 §3.1.2.5). Fixed depth; enqueue fails when
// full (drop-new). Broker owns pool/topic refcounts — entries are plain copies.
// Flushed into the transport TxQueue on reconnect via flush_offline_queue().

#include "mqtt_broker/pending_delivery.hpp"

#include <cstddef>
#include <cstdint>

namespace mqtt_broker {

struct OfflineQueueConfig {
    size_t depth;  // Max queued deliveries per session slot
};

// Ring buffer of PendingDelivery. Not thread-safe; one queue per session slot.
class OfflineQueue {
public:
    explicit OfflineQueue(const OfflineQueueConfig &cfg);
    ~OfflineQueue();

    OfflineQueue(const OfflineQueue &) = delete;
    OfflineQueue &operator=(const OfflineQueue &) = delete;

    // Returns false when full or allocation failed (drop-new quota policy).
    bool enqueue(const PendingDelivery &delivery);
    // Oldest entry without removing it; caller releases resources after pop().
    bool peek(PendingDelivery *out) const;
    bool pop();
    void clear();  // Resets indices only — caller must release entry handles first
    size_t count() const { return count_; }
    bool full() const { return count_ >= depth_; }

private:
    PendingDelivery *entries_;
    size_t depth_;
    size_t head_;  // Dequeue index
    size_t tail_;  // Next enqueue index
    size_t count_;
};

}  // namespace mqtt_broker

#endif
