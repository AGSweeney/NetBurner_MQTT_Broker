// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#include "mqtt_broker/tx_queue.hpp"

// TxQueue implementation — ring buffer plus logical-byte accounting shared with
// Broker global_logical_tx_ admission checks.

#include <cstring>
#include <new>

namespace mqtt_broker {

TxQueue::TxQueue(const TxQueueConfig &cfg)
    : entries_(nullptr),
      depth_(cfg.depth),
      head_(0),
      tail_(0),
      count_(0),
      logical_bytes_(0),
      logical_limit_(cfg.logical_limit),
      logical_high_water_(0)
{
    entries_ = new (std::nothrow) PendingDelivery[depth_];
}

bool TxQueue::enqueue(const PendingDelivery &delivery, uint32_t encoded_size)
{
    if (entries_ == nullptr || count_ >= depth_) {
        return false;
    }
    if (logical_bytes_ + encoded_size > logical_limit_) {
        return false;  // Per-connection TX budget exhausted before depth fills
    }
    entries_[tail_] = delivery;
    entries_[tail_].encoded_total_size = encoded_size;
    tail_ = (tail_ + 1) % depth_;
    count_++;
    logical_bytes_ += encoded_size;
    if (logical_bytes_ > logical_high_water_) {
        logical_high_water_ = logical_bytes_;
    }
    return true;
}

bool TxQueue::peek(PendingDelivery *out) const
{
    if (out == nullptr || count_ == 0) {
        return false;
    }
    *out = entries_[head_];
    return true;
}

bool TxQueue::pop(uint32_t *released_logical)
{
    if (count_ == 0) {
        return false;
    }
    if (released_logical != nullptr) {
        *released_logical = entries_[head_].encoded_total_size;
    }
    if (logical_bytes_ >= entries_[head_].encoded_total_size) {
        logical_bytes_ -= entries_[head_].encoded_total_size;
    } else {
        logical_bytes_ = 0;  // Guard against accounting drift
    }
    head_ = (head_ + 1) % depth_;
    count_--;
    return true;
}

bool TxQueue::advance_offset(uint32_t delta)
{
    if (count_ == 0) {
        return false;
    }
    // Only the head entry is partially encoded during drain_tx().
    entries_[head_].encoded_offset += delta;
    return true;
}

uint32_t TxQueue::front_offset() const
{
    if (count_ == 0) {
        return 0;
    }
    return entries_[head_].encoded_offset;
}

}  // namespace mqtt_broker
