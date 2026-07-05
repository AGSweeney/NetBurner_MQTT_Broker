// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// RetainedStore implementation — upsert/delete by topic with MessagePool and
// TopicInternPool ref ownership; in-place replace reuses the topic handle.

#include "mqtt_broker/retained_store.hpp"

#include <new>

namespace mqtt_broker {

RetainedStore::RetainedStore(const RetainedStoreConfig &cfg)
    : entries_(nullptr),
      cap_(cfg.max_entries),
      count_(0),
      bytes_used_(0),
      byte_limit_(cfg.max_bytes)
{
    entries_ = new (std::nothrow) Entry[cap_];
    if (entries_ != nullptr) {
        for (size_t i = 0; i < cap_; ++i) {
            entries_[i].in_use = false;
            entries_[i].topic = NULL_TOPIC;
            entries_[i].message = NULL_MESSAGE;
        }
    }
}

RetainedStore::~RetainedStore()
{
    delete[] entries_;
}

bool RetainedStore::put(TopicHandle topic, MessageHandle msg, MessagePool *pool,
                        TopicInternPool *intern)
{
    if (entries_ == nullptr || pool == nullptr || intern == nullptr || !intern->valid(topic) ||
        !pool->msg_valid(msg)) {
        return false;
    }

    size_t payload_len = pool->payload_size(msg);

    // In-place replace: release old message ref, keep topic ref unchanged.
    for (size_t i = 0; i < cap_; ++i) {
        if (entries_[i].in_use && entries_[i].topic.slot == topic.slot &&
            entries_[i].topic.generation == topic.generation) {
            size_t old_len = pool->payload_size(entries_[i].message);
            if (payload_len > old_len &&
                bytes_used_ - old_len + payload_len > byte_limit_) {
                return false;
            }
            pool->msg_release(entries_[i].message);
            if (!pool->msg_acquire(msg)) {
                entries_[i].in_use = false;
                count_--;
                return false;
            }
            entries_[i].message = msg;
            if (bytes_used_ >= old_len) {
                bytes_used_ -= static_cast<uint32_t>(old_len);
            }
            bytes_used_ += static_cast<uint32_t>(payload_len);
            return true;
        }
    }

    if (bytes_used_ + payload_len > byte_limit_) {
        return false;
    }

    size_t slot = cap_;
    for (size_t i = 0; i < cap_; ++i) {
        if (!entries_[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot >= cap_) {
        return false;
    }

    if (!pool->msg_acquire(msg)) {
        return false;
    }
    if (!intern->acquire(topic)) {
        pool->msg_release(msg);
        return false;
    }

    entries_[slot].in_use = true;
    entries_[slot].topic = topic;
    entries_[slot].message = msg;
    count_++;
    bytes_used_ += static_cast<uint32_t>(payload_len);
    return true;
}

MessageHandle RetainedStore::get(TopicHandle topic) const
{
    if (entries_ == nullptr) {
        return NULL_MESSAGE;
    }
    for (size_t i = 0; i < cap_; ++i) {
        if (entries_[i].in_use && entries_[i].topic.slot == topic.slot &&
            entries_[i].topic.generation == topic.generation) {
            return entries_[i].message;
        }
    }
    return NULL_MESSAGE;
}

bool RetainedStore::entry_at(size_t index, TopicHandle *topic, MessageHandle *msg) const
{
    if (entries_ == nullptr || index >= cap_ || topic == nullptr || msg == nullptr) {
        return false;
    }
    if (!entries_[index].in_use) {
        return false;
    }
    *topic = entries_[index].topic;
    *msg = entries_[index].message;
    return true;
}

void RetainedStore::remove(TopicHandle topic, MessagePool *pool, TopicInternPool *intern)
{
    if (entries_ == nullptr || pool == nullptr) {
        return;
    }
    for (size_t i = 0; i < cap_; ++i) {
        if (entries_[i].in_use && entries_[i].topic.slot == topic.slot &&
            entries_[i].topic.generation == topic.generation) {
            size_t len = pool->payload_size(entries_[i].message);
            pool->msg_release(entries_[i].message);
            if (intern != nullptr) {
                intern->release(entries_[i].topic);
            }
            entries_[i].in_use = false;
            entries_[i].message = NULL_MESSAGE;
            entries_[i].topic = NULL_TOPIC;
            if (count_ > 0) {
                count_--;
            }
            if (bytes_used_ >= len) {
                bytes_used_ -= static_cast<uint32_t>(len);
            }
            return;
        }
    }
}

}  // namespace mqtt_broker
