// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// Topic string interning pool. Deduplicates topic/filter names into a fixed
// blob so subscriptions, retained entries, and inflight queues share one copy
// per distinct topic. Handles carry a generation counter so stale references
// are rejected after the slot is recycled.

#ifndef MQTT_BROKER_TOPIC_INTERN_HPP
#define MQTT_BROKER_TOPIC_INTERN_HPP

#include <cstddef>
#include <cstdint>

namespace mqtt_broker {

// Opaque reference to an interned topic. Valid only while ref_count > 0 and
// generation matches the slot (checked by valid()).
struct TopicHandle {
    uint16_t slot;        // Index into entries_
    uint16_t generation;  // Bumped when slot is freed and reused
};

static const TopicHandle NULL_TOPIC = {0xFFFF, 0xFFFF};  // intern()/acquire() failure sentinel

struct TopicInternConfig {
    size_t max_topics;     // Distinct interned strings (entry table size)
    size_t max_bytes;      // Total bytes in the topic blob (incl. NUL terminators)
    size_t max_topic_len;  // Reserved; enforced by TopicTrie::topic_valid at intern time
};

// Reference-counted intern pool. intern() deduplicates; acquire()/release()
// share ownership across trie nodes, retained store, and inflight queues.
class TopicInternPool {
public:
    explicit TopicInternPool(const TopicInternConfig &cfg);
    ~TopicInternPool();

    TopicInternPool(const TopicInternPool &) = delete;
    TopicInternPool &operator=(const TopicInternPool &) = delete;

    // Return existing handle or insert topic; bumps ref_count on success.
    TopicHandle intern(const char *topic);
    // Increment ref_count for an already-interned handle (e.g. retained put).
    bool acquire(TopicHandle h);
    // Decrement ref_count; frees slot and bumps generation when count hits zero.
    void release(TopicHandle h);
    bool valid(TopicHandle h) const;
    const char *c_str(TopicHandle h) const;
    uint16_t ref_count(TopicHandle h) const;

    size_t topic_count() const { return topic_count_; }
    size_t bytes_used() const { return bytes_used_; }

private:
    struct Entry {
        uint16_t generation;
        uint16_t ref_count;
        uint16_t offset;  // Byte offset into blob_
        uint16_t length;  // Topic length excluding NUL
        bool in_use;
    };

    Entry *entries_;
    size_t entry_cap_;
    char *blob_;
    size_t blob_cap_;
    size_t topic_count_;
    size_t bytes_used_;
};

}  // namespace mqtt_broker

#endif
