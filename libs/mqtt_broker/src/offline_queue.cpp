#include "mqtt_broker/offline_queue.hpp"

// Fixed-capacity ring buffer backing store for offline session deliveries.
// Allocation uses nothrow so broker startup survives low-memory targets.

#include <new>

namespace mqtt_broker {

OfflineQueue::OfflineQueue(const OfflineQueueConfig &cfg)
    : entries_(nullptr),
      depth_(cfg.depth),
      head_(0),
      tail_(0),
      count_(0)
{
    entries_ = new (std::nothrow) PendingDelivery[depth_];
}

OfflineQueue::~OfflineQueue()
{
    delete[] entries_;
}

bool OfflineQueue::enqueue(const PendingDelivery &delivery)
{
    if (entries_ == nullptr || count_ >= depth_) {
        return false;  // Drop-new: broker increments dropped_quota on false
    }
    entries_[tail_] = delivery;
    tail_ = (tail_ + 1) % depth_;
    count_++;
    return true;
}

bool OfflineQueue::peek(PendingDelivery *out) const
{
    if (out == nullptr || count_ == 0) {
        return false;
    }
    *out = entries_[head_];
    return true;
}

bool OfflineQueue::pop()
{
    if (count_ == 0) {
        return false;
    }
    head_ = (head_ + 1) % depth_;
    count_--;
    return true;
}

void OfflineQueue::clear()
{
    // Logical empty — broker clears session state after releasing entries.
    head_ = 0;
    tail_ = 0;
    count_ = 0;
}

}  // namespace mqtt_broker
