// MessagePool implementation — block-chained payload arena with generation-
// stamped ref-counted slots. Blocks are recycled through an intrusive free list;
// slot generation increments on final release so stale handles fail msg_valid().

#include "mqtt_broker/message_pool.hpp"

#include <cstring>
#include <new>

namespace mqtt_broker {

MessagePool::MessagePool(const MessagePoolConfig &cfg)
    : block_size_(cfg.block_size),
      block_count_(cfg.block_count),
      slots_(nullptr),
      slot_count_(cfg.slot_count != 0 ? cfg.slot_count : cfg.block_count),
      blocks_(nullptr),
      block_data_(nullptr),
      free_block_head_(0xFFFF),
      blocks_allocated_(0),
      blocks_free_(0)
{
    slots_ = new (std::nothrow) Slot[slot_count_];
    blocks_ = new (std::nothrow) Block[block_count_];
    block_data_ = new (std::nothrow) uint8_t[block_count_ * block_size_];

    if (slots_ == nullptr || blocks_ == nullptr || block_data_ == nullptr) {
        return;
    }

    for (size_t i = 0; i < slot_count_; ++i) {
        slots_[i].generation = 1;
        slots_[i].ref_count = 0;
        slots_[i].payload_len = 0;
        slots_[i].first_block = 0xFFFF;
        slots_[i].block_count = 0;
        slots_[i].in_use = false;
        slots_[i].props_len = 0;
        slots_[i].expiry_interval = 0;
        slots_[i].publish_tick = 0;
    }

    // Seed the block free list in index order.
    blocks_free_ = block_count_;
    for (size_t i = 0; i < block_count_; ++i) {
        blocks_[i].in_use = false;
        blocks_[i].next = (i + 1 < block_count_) ? static_cast<uint16_t>(i + 1) : 0xFFFF;
    }
    free_block_head_ = block_count_ > 0 ? 0 : 0xFFFF;
}

MessagePool::~MessagePool()
{
    delete[] slots_;
    delete[] blocks_;
    delete[] block_data_;
}

MessageHandle MessagePool::acquire_empty()
{
    for (size_t i = 0; i < slot_count_; ++i) {
        if (!slots_[i].in_use) {
            slots_[i].in_use = true;
            slots_[i].ref_count = 1;
            slots_[i].payload_len = 0;
            slots_[i].first_block = 0xFFFF;
            slots_[i].block_count = 0;
            slots_[i].props_len = 0;
            slots_[i].expiry_interval = 0;
            slots_[i].publish_tick = 0;
            MessageHandle h = {static_cast<uint16_t>(i), slots_[i].generation};
            return h;
        }
    }
    return NULL_MESSAGE;
}

bool MessagePool::msg_valid(MessageHandle h) const
{
    if (!message_handle_valid(h) || h.slot >= slot_count_) {
        return false;
    }
    const Slot &s = slots_[h.slot];
    return s.in_use && s.generation == h.generation;
}

bool MessagePool::msg_acquire(MessageHandle h)
{
    if (!msg_valid(h)) {
        return false;
    }
    if (slots_[h.slot].ref_count == 0xFFFF) {
        return false;
    }
    slots_[h.slot].ref_count++;
    return true;
}

void MessagePool::msg_release(MessageHandle h)
{
    if (!msg_valid(h)) {
        return;
    }
    Slot &s = slots_[h.slot];
    if (s.ref_count == 0) {
        return;
    }
    s.ref_count--;
    if (s.ref_count > 0) {
        return;
    }

    // Walk the block chain back onto the free list.
    uint16_t blk = s.first_block;
    while (blk != 0xFFFF && blk < block_count_) {
        uint16_t next = blocks_[blk].next;
        blocks_[blk].in_use = false;
        blocks_[blk].next = free_block_head_;
        free_block_head_ = blk;
        blocks_allocated_--;
        blocks_free_++;
        blk = next;
    }

    s.in_use = false;
    s.payload_len = 0;
    s.first_block = 0xFFFF;
    s.block_count = 0;
    s.props_len = 0;
    s.expiry_interval = 0;
    s.publish_tick = 0;
    // Bump generation so outstanding handles become invalid.
    s.generation++;
    if (s.generation == 0) {
        s.generation = 1;
    }
}

bool MessagePool::set_props(MessageHandle h, const uint8_t *data, size_t len)
{
    if (!msg_valid(h) || data == nullptr || len > kMessagePropsCapacity) {
        return false;
    }
    Slot &s = slots_[h.slot];
    if (len > 0) {
        std::memcpy(s.props, data, len);
    }
    s.props_len = static_cast<uint16_t>(len);
    return true;
}

const uint8_t *MessagePool::props(MessageHandle h, size_t *out_len) const
{
    if (!msg_valid(h) || slots_[h.slot].props_len == 0) {
        if (out_len) {
            *out_len = 0;
        }
        return nullptr;
    }
    if (out_len) {
        *out_len = slots_[h.slot].props_len;
    }
    return slots_[h.slot].props;
}

void MessagePool::set_expiry(MessageHandle h, uint32_t interval_sec, uint32_t publish_tick)
{
    if (!msg_valid(h)) {
        return;
    }
    slots_[h.slot].expiry_interval = interval_sec;
    slots_[h.slot].publish_tick = publish_tick;
}

uint32_t MessagePool::expiry_interval(MessageHandle h) const
{
    if (!msg_valid(h)) {
        return 0;
    }
    return slots_[h.slot].expiry_interval;
}

uint32_t MessagePool::publish_tick(MessageHandle h) const
{
    if (!msg_valid(h)) {
        return 0;
    }
    return slots_[h.slot].publish_tick;
}

uint16_t MessagePool::msg_ref_count(MessageHandle h) const
{
    if (!msg_valid(h)) {
        return 0;
    }
    return slots_[h.slot].ref_count;
}

uint16_t MessagePool::tail_block(uint16_t first) const
{
    uint16_t cur = first;
    while (cur != 0xFFFF && blocks_[cur].next != 0xFFFF) {
        cur = blocks_[cur].next;
    }
    return cur;
}

bool MessagePool::append_block(MessageHandle h, const uint8_t *data, size_t len)
{
    if (!msg_valid(h) || data == nullptr || len == 0 || len > block_size_) {
        return false;
    }
    if (free_block_head_ == 0xFFFF) {
        return false;
    }

    uint16_t blk = free_block_head_;
    free_block_head_ = blocks_[blk].next;
    blocks_[blk].in_use = true;
    blocks_[blk].next = 0xFFFF;
    std::memcpy(block_data_ + blk * block_size_, data, len);

    Slot &s = slots_[h.slot];
    if (s.first_block == 0xFFFF) {
        s.first_block = blk;
    } else {
        uint16_t cur = tail_block(s.first_block);
        blocks_[cur].next = blk;
    }
    s.block_count++;
    s.payload_len += static_cast<uint32_t>(len);
    blocks_allocated_++;
    blocks_free_--;
    return true;
}

bool MessagePool::append_payload(MessageHandle h, const uint8_t *data, size_t len)
{
    if (!msg_valid(h) || data == nullptr || len == 0) {
        return false;
    }

    Slot &s = slots_[h.slot];
    size_t offset = 0;
    while (offset < len) {
        // Prefer filling the partial tail block before taking a new one.
        size_t partial = static_cast<size_t>(s.payload_len % block_size_);
        if (partial != 0 && s.first_block != 0xFFFF) {
            uint16_t tail = tail_block(s.first_block);
            size_t space = block_size_ - partial;
            size_t chunk = len - offset;
            if (chunk > space) {
                chunk = space;
            }
            std::memcpy(block_data_ + tail * block_size_ + partial, data + offset, chunk);
            s.payload_len += static_cast<uint32_t>(chunk);
            offset += chunk;
            continue;
        }

        size_t chunk = len - offset;
        if (chunk > block_size_) {
            chunk = block_size_;
        }
        if (!append_block(h, data + offset, chunk)) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

size_t MessagePool::payload_size(MessageHandle h) const
{
    if (!msg_valid(h)) {
        return 0;
    }
    return slots_[h.slot].payload_len;
}

size_t MessagePool::read_payload(MessageHandle h, size_t offset, uint8_t *out, size_t cap) const
{
    if (!msg_valid(h) || out == nullptr || cap == 0) {
        return 0;
    }
    const Slot &s = slots_[h.slot];
    if (offset >= s.payload_len) {
        return 0;
    }

    size_t to_copy = static_cast<size_t>(s.payload_len) - offset;
    if (to_copy > cap) {
        to_copy = cap;
    }
    size_t copied = 0;
    uint16_t blk = s.first_block;
    size_t pos = 0;

    while (blk != 0xFFFF && blk < block_count_ && copied < to_copy) {
        size_t block_end = pos + block_size_;
        if (offset >= block_end) {
            pos = block_end;
            blk = blocks_[blk].next;
            continue;
        }
        size_t start_in_block = offset > pos ? offset - pos : 0;
        size_t avail = block_size_ - start_in_block;
        size_t chunk = to_copy - copied;
        if (chunk > avail) {
            chunk = avail;
        }
        std::memcpy(out + copied, block_data_ + blk * block_size_ + start_in_block, chunk);
        copied += chunk;
        offset += chunk;
        pos += block_size_;
        blk = blocks_[blk].next;
    }
    return copied;
}

uint8_t *MessagePool::block_data(MessageHandle h, size_t *out_len)
{
    if (!msg_valid(h)) {
        return nullptr;
    }
    Slot &s = slots_[h.slot];
    if (s.first_block == 0xFFFF) {
        if (out_len) {
            *out_len = 0;
        }
        return nullptr;
    }
    if (out_len) {
        *out_len = s.payload_len;
    }
    return block_data_ + s.first_block * block_size_;
}

}  // namespace mqtt_broker
