// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// TopicInternPool implementation — deduplicated topic strings in a shared blob
// arena with ref-counted TopicHandle slots and generation invalidation on release.

#include "mqtt_broker/topic_intern.hpp"

#include "mqtt_broker/topic_trie.hpp"

#include <cstring>

namespace mqtt_broker {

TopicInternPool::TopicInternPool(const TopicInternConfig &cfg)
    : entries_(nullptr),
      entry_cap_(cfg.max_topics),
      blob_(nullptr),
      blob_cap_(cfg.max_bytes),
      topic_count_(0),
      bytes_used_(0)
{
    entries_ = new Entry[entry_cap_];
    blob_ = new char[blob_cap_];
    for (size_t i = 0; i < entry_cap_; ++i) {
        entries_[i].generation = 1;  // 0 reserved — would collide with wrap guard
        entries_[i].ref_count = 0;
        entries_[i].in_use = false;
    }
}

TopicInternPool::~TopicInternPool()
{
    delete[] entries_;
    delete[] blob_;
}

bool TopicInternPool::valid(TopicHandle h) const
{
    if (h.slot == 0xFFFF || h.slot >= entry_cap_) {
        return false;
    }
    const Entry &e = entries_[h.slot];
    return e.in_use && e.generation == h.generation;
}

TopicHandle TopicInternPool::intern(const char *topic)
{
    if (topic == nullptr || !TopicTrie::topic_valid(topic)) {
        return NULL_TOPIC;
    }

    size_t len = std::strlen(topic);
    if (len == 0 || bytes_used_ + len + 1 > blob_cap_) {
        return NULL_TOPIC;
    }

    // Dedup: same bytes already in the blob — reuse slot and bump ref_count.
    for (size_t i = 0; i < entry_cap_; ++i) {
        if (entries_[i].in_use && entries_[i].length == len &&
            std::memcmp(blob_ + entries_[i].offset, topic, len) == 0) {
            entries_[i].ref_count++;
            return {static_cast<uint16_t>(i), entries_[i].generation};
        }
    }

    // New topic: claim a free slot and append to the blob (NUL-terminated).
    for (size_t i = 0; i < entry_cap_; ++i) {
        if (!entries_[i].in_use) {
            entries_[i].in_use = true;
            entries_[i].ref_count = 1;
            entries_[i].offset = static_cast<uint16_t>(bytes_used_);
            entries_[i].length = static_cast<uint16_t>(len);
            std::memcpy(blob_ + bytes_used_, topic, len);
            blob_[bytes_used_ + len] = '\0';
            bytes_used_ += len + 1;
            topic_count_++;
            return {static_cast<uint16_t>(i), entries_[i].generation};
        }
    }

    return NULL_TOPIC;
}

bool TopicInternPool::acquire(TopicHandle h)
{
    if (!valid(h)) {
        return false;
    }
    entries_[h.slot].ref_count++;
    return true;
}

void TopicInternPool::release(TopicHandle h)
{
    if (!valid(h)) {
        return;
    }
    Entry &e = entries_[h.slot];
    if (e.ref_count == 0) {
        return;
    }
    e.ref_count--;
    if (e.ref_count > 0) {
        return;
    }
    e.in_use = false;
    topic_count_--;
    // Bump generation so any outstanding handle becomes invalid without scanning.
    e.generation++;
    if (e.generation == 0) {
        e.generation = 1;
    }
}

uint16_t TopicInternPool::ref_count(TopicHandle h) const
{
    if (!valid(h)) {
        return 0;
    }
    return entries_[h.slot].ref_count;
}

const char *TopicInternPool::c_str(TopicHandle h) const
{
    if (!valid(h)) {
        return "";
    }
    return blob_ + entries_[h.slot].offset;
}

}  // namespace mqtt_broker
