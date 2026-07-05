// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// Fixed-capacity retained message store (MQTT 5 §3.3.1.3). One entry per topic
// name; PUBLISH with RETAIN=1 upserts, zero-length payload deletes. Payload
// bytes and topic strings are owned via MessagePool and TopicInternPool ref
// counts. Broker enforces entry/byte limits and calls remove on expiry sweep.

#ifndef MQTT_BROKER_RETAINED_STORE_HPP
#define MQTT_BROKER_RETAINED_STORE_HPP

#include "mqtt_broker/message_pool.hpp"
#include "mqtt_broker/topic_intern.hpp"

#include <cstddef>

namespace mqtt_broker {

struct RetainedStoreConfig {
    size_t max_entries;  // Distinct retained topics
    uint32_t max_bytes;  // Sum of retained payload sizes
};

// Retained PUBLISH cache keyed by interned topic handle. put()/remove() adjust
// pool ref counts; get() returns a handle without acquiring (caller must not release).
class RetainedStore {
public:
    explicit RetainedStore(const RetainedStoreConfig &cfg);
    ~RetainedStore();

    RetainedStore(const RetainedStore &) = delete;
    RetainedStore &operator=(const RetainedStore &) = delete;

    // Upsert retained message for topic. Replaces existing entry in place when
    // present; acquires msg and topic refs on insert. Returns false on limit breach.
    bool put(TopicHandle topic, MessageHandle msg, MessagePool *pool, TopicInternPool *intern);
    // Lookup by topic handle; NULL_MESSAGE if none. Does not acquire the message.
    MessageHandle get(TopicHandle topic) const;
    // Drop retained entry and release message/topic refs (§3.3.1.3 zero-length delete).
    void remove(TopicHandle topic, MessagePool *pool, TopicInternPool *intern);

    size_t entry_count() const { return count_; }
    uint32_t bytes_used() const { return bytes_used_; }

    // Iterate in-use slots by table index (for retained delivery and expiry sweep).
    bool entry_at(size_t index, TopicHandle *topic, MessageHandle *msg) const;

private:
    struct Entry {
        TopicHandle topic;
        MessageHandle message;
        bool in_use;
    };

    Entry *entries_;
    size_t cap_;
    size_t count_;
    uint32_t bytes_used_;
    uint32_t byte_limit_;
};

}  // namespace mqtt_broker

#endif
