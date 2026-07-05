// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// PropertyPool implementation — slot-partitioned records over an append-only
// blob arena. Values are stored in MQTT wire byte order so encode can re-serialize
// without re-parsing. Blob space is not reclaimed on release; broker lifetime
// must fit within max_blob_bytes.

#include "mqtt_broker/property_pool.hpp"

#include <cstring>
#include <new>

namespace mqtt_broker {

static const size_t kPropsPerSlot = 16;

PropertyPool::PropertyPool(const PropertyPoolConfig &cfg)
    : slots_(nullptr),
      slot_cap_(cfg.max_slots),
      records_(nullptr),
      record_cap_(cfg.max_slots * kPropsPerSlot),
      blob_(nullptr),
      blob_cap_(cfg.max_blob_bytes),
      bytes_used_(0),
      free_record_head_(0xFFFF)
{
    slots_ = new (std::nothrow) Slot[slot_cap_];
    records_ = new (std::nothrow) PropertyRecord[record_cap_];
    blob_ = new (std::nothrow) uint8_t[blob_cap_];

    if (slots_ == nullptr || records_ == nullptr || blob_ == nullptr) {
        return;
    }

    for (size_t i = 0; i < slot_cap_; ++i) {
        slots_[i].generation = 1;
        slots_[i].property_count = 0;
        slots_[i].first_record = static_cast<uint16_t>(i * kPropsPerSlot);
        slots_[i].in_use = false;
    }
}

PropertyPool::~PropertyPool()
{
    delete[] slots_;
    delete[] records_;
    delete[] blob_;
}

bool PropertyPool::valid(PropertyHandle h) const
{
    if (h.slot == 0xFFFF || h.slot >= slot_cap_) {
        return false;
    }
    const Slot &s = slots_[h.slot];
    return s.in_use && s.generation == h.generation;
}

PropertyHandle PropertyPool::acquire()
{
    for (size_t i = 0; i < slot_cap_; ++i) {
        if (!slots_[i].in_use) {
            slots_[i].in_use = true;
            slots_[i].property_count = 0;
            return {static_cast<uint16_t>(i), slots_[i].generation};
        }
    }
    return NULL_PROPERTY;
}

void PropertyPool::release(PropertyHandle h)
{
    if (!valid(h)) {
        return;
    }
    Slot &s = slots_[h.slot];
    s.in_use = false;
    s.property_count = 0;
    // Bump generation; blob offsets from this slot are now orphaned.
    s.generation++;
    if (s.generation == 0) {
        s.generation = 1;
    }
}

bool PropertyPool::append_blob(const uint8_t *data, size_t len, uint16_t *out_offset)
{
    if (bytes_used_ + len > blob_cap_ || len > 0xFFFFu) {
        return false;
    }
    *out_offset = static_cast<uint16_t>(bytes_used_);
    if (len > 0 && data != nullptr) {
        std::memcpy(blob_ + bytes_used_, data, len);
    }
    bytes_used_ += len;
    return true;
}

bool PropertyPool::add_record(PropertyHandle h, PropertyId id, PropertyValueKind kind,
                              uint16_t offset, uint16_t length, uint16_t key_offset,
                              uint16_t key_length)
{
    if (!valid(h)) {
        return false;
    }
    Slot &s = slots_[h.slot];
    if (s.property_count >= kPropsPerSlot) {
        return false;
    }

    // Records are pre-partitioned by slot to avoid a separate free list.
    size_t rec_index = static_cast<size_t>(h.slot) * kPropsPerSlot + s.property_count;
    if (rec_index >= record_cap_) {
        return false;
    }

    PropertyRecord &r = records_[rec_index];
    r.id = id;
    r.kind = kind;
    r.offset = offset;
    r.length = length;
    r.key_offset = key_offset;
    r.key_length = key_length;
    s.property_count++;
    return true;
}

bool PropertyPool::add_byte(PropertyHandle h, PropertyId id, uint8_t value)
{
    uint16_t off = 0;
    if (!append_blob(&value, 1, &off)) {
        return false;
    }
    return add_record(h, id, PropertyValueKind::Byte, off, 1);
}

bool PropertyPool::add_u16(PropertyHandle h, PropertyId id, uint16_t value)
{
    uint8_t buf[2] = {static_cast<uint8_t>((value >> 8) & 0xFFu),
                      static_cast<uint8_t>(value & 0xFFu)};
    uint16_t off = 0;
    if (!append_blob(buf, 2, &off)) {
        return false;
    }
    return add_record(h, id, PropertyValueKind::U16, off, 2);
}

bool PropertyPool::add_u32(PropertyHandle h, PropertyId id, uint32_t value)
{
    uint8_t buf[4] = {
        static_cast<uint8_t>((value >> 24) & 0xFFu), static_cast<uint8_t>((value >> 16) & 0xFFu),
        static_cast<uint8_t>((value >> 8) & 0xFFu), static_cast<uint8_t>(value & 0xFFu)};
    uint16_t off = 0;
    if (!append_blob(buf, 4, &off)) {
        return false;
    }
    return add_record(h, id, PropertyValueKind::U32, off, 4);
}

bool PropertyPool::add_varint(PropertyHandle h, PropertyId id, uint32_t value)
{
    uint8_t buf[4];
    size_t n = 0;
    uint32_t v = value;
    do {
        uint8_t byte = static_cast<uint8_t>(v % 128u);
        v /= 128u;
        if (v > 0) {
            byte = static_cast<uint8_t>(byte | 0x80u);
        }
        buf[n++] = byte;
    } while (v > 0 && n < sizeof(buf));

    uint16_t off = 0;
    if (!append_blob(buf, n, &off)) {
        return false;
    }
    return add_record(h, id, PropertyValueKind::Varint, off, static_cast<uint16_t>(n));
}

bool PropertyPool::add_utf8(PropertyHandle h, PropertyId id, const char *str, size_t len)
{
    if (str == nullptr) {
        return false;
    }
    uint16_t off = 0;
    if (!append_blob(reinterpret_cast<const uint8_t *>(str), len, &off)) {
        return false;
    }
    return add_record(h, id, PropertyValueKind::Utf8String, off, static_cast<uint16_t>(len));
}

bool PropertyPool::add_binary(PropertyHandle h, PropertyId id, const uint8_t *data, size_t len)
{
    if (data == nullptr) {
        return false;
    }
    uint16_t off = 0;
    if (!append_blob(data, len, &off)) {
        return false;
    }
    return add_record(h, id, PropertyValueKind::Binary, off, static_cast<uint16_t>(len));
}

bool PropertyPool::add_user_property(PropertyHandle h, const char *key, size_t key_len,
                                     const char *val, size_t val_len)
{
    if (key == nullptr || val == nullptr) {
        return false;
    }
    uint16_t key_off = 0;
    if (!append_blob(reinterpret_cast<const uint8_t *>(key), key_len, &key_off)) {
        return false;
    }
    uint16_t val_off = 0;
    if (!append_blob(reinterpret_cast<const uint8_t *>(val), val_len, &val_off)) {
        return false;
    }
    return add_record(h, UserProperty, PropertyValueKind::UserProperty, val_off,
                      static_cast<uint16_t>(val_len), key_off, static_cast<uint16_t>(key_len));
}

size_t PropertyPool::count(PropertyHandle h) const
{
    if (!valid(h)) {
        return 0;
    }
    return slots_[h.slot].property_count;
}

const PropertyRecord *PropertyPool::records(PropertyHandle h, size_t *out_count) const
{
    if (!valid(h)) {
        if (out_count) {
            *out_count = 0;
        }
        return nullptr;
    }
    if (out_count) {
        *out_count = slots_[h.slot].property_count;
    }
    size_t base = static_cast<size_t>(h.slot) * kPropsPerSlot;
    if (base >= record_cap_) {
        return nullptr;
    }
    return &records_[base];
}

}  // namespace mqtt_broker
