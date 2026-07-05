// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// Ref-counted PUBLISH payload storage backed by a fixed block pool. Payloads
// chain across blocks so retained messages, inflight QoS deliveries, and offline
// queues share memory without per-message heap allocation. Forwardable PUBLISH
// properties and Message Expiry Interval (MQTT-5.0 §2.2.2.2) ride with the
// payload for re-encoding at delivery time.

#ifndef MQTT_BROKER_MESSAGE_POOL_HPP
#define MQTT_BROKER_MESSAGE_POOL_HPP

#include "mqtt_broker/types.hpp"

#include <cstddef>
#include <cstdint>

namespace mqtt_broker {

// Max wire bytes for forwardable PUBLISH properties copied into each slot.
static const size_t kMessagePropsCapacity = 256;

// Block-pool sizing. slot_count defaults to block_count when zero.
struct MessagePoolConfig {
    size_t block_size;   // Bytes per payload block in the arena
    size_t block_count;  // Total blocks shared across all messages
    size_t slot_count;   // 0 => use block_count
};

// Generation-stamped handles (MessageHandle) identify live slots. ref_count
// tracks shared ownership across subscribers, inflight tables, and retained
// store; msg_release() frees blocks when the count reaches zero.
class MessagePool {
public:
    explicit MessagePool(const MessagePoolConfig &cfg);
    ~MessagePool();

    MessagePool(const MessagePool &) = delete;
    MessagePool &operator=(const MessagePool &) = delete;

    // Returns a new handle with ref_count == 1, or NULL_MESSAGE when slots
    // are exhausted. Caller must msg_release() exactly once per acquire.
    MessageHandle acquire_empty();
    // Increment ref_count; returns false if handle is stale or at max refs.
    bool msg_acquire(MessageHandle h);
    // Decrement ref_count; recycles slot and payload blocks at zero.
    void msg_release(MessageHandle h);
    bool msg_valid(MessageHandle h) const;
    uint16_t msg_ref_count(MessageHandle h) const;

    // Direct pointer to the first block's storage. Safe only when the entire
    // payload fits in one block; use read_payload() for multi-block messages.
    uint8_t *block_data(MessageHandle h, size_t *out_len);
    size_t payload_size(MessageHandle h) const;
    size_t read_payload(MessageHandle h, size_t offset, uint8_t *out, size_t cap) const;
    // Append one full block (len must be <= block_size).
    bool append_block(MessageHandle h, const uint8_t *data, size_t len);
    // Append arbitrary length; fills the tail of the current block before
    // allocating new ones.
    bool append_payload(MessageHandle h, const uint8_t *data, size_t len);

    // Forwardable PUBLISH properties, stored as pre-serialized wire bytes.
    bool set_props(MessageHandle h, const uint8_t *data, size_t len);
    const uint8_t *props(MessageHandle h, size_t *out_len) const;

    // Message Expiry Interval bookkeeping (MQTT-5.0 §2.2.2.2). interval 0
    // means "no expiry"; publish_tick is the broker tick when stored.
    void set_expiry(MessageHandle h, uint32_t interval_sec, uint32_t publish_tick);
    uint32_t expiry_interval(MessageHandle h) const;
    uint32_t publish_tick(MessageHandle h) const;

    size_t blocks_allocated() const { return blocks_allocated_; }
    size_t blocks_free() const { return blocks_free_; }

private:
    struct Slot {
        uint16_t generation;     // Bumped on release to invalidate stale handles
        uint16_t ref_count;
        uint32_t payload_len;    // Total bytes across all chained blocks
        uint16_t first_block;    // Head of block chain; 0xFFFF when empty
        uint16_t block_count;
        bool in_use;
        uint16_t props_len;
        uint32_t expiry_interval;
        uint32_t publish_tick;
        uint8_t props[kMessagePropsCapacity];
    };

    struct Block {
        bool in_use;
        uint16_t next;  // Next block in chain; 0xFFFF at tail
    };

    size_t block_size_;
    size_t block_count_;
    Slot *slots_;
    size_t slot_count_;
    Block *blocks_;
    uint8_t *block_data_;
    uint16_t free_block_head_;  // Intrusive free list through Block::next
    size_t blocks_allocated_;
    size_t blocks_free_;

    uint16_t tail_block(uint16_t first) const;
};

}  // namespace mqtt_broker

#endif
