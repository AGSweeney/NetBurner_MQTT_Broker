// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// Parsed MQTT 5 property sets for inbound packets (CONNECT, PUBLISH, etc.).
// Each PropertyHandle owns a fixed slice of PropertyRecord entries plus offsets
// into a shared append-only blob arena. The parser builds properties here;
// encode serializes them back to wire form (MQTT-5.0 §1.5).

#ifndef MQTT_BROKER_PROPERTY_POOL_HPP
#define MQTT_BROKER_PROPERTY_POOL_HPP

#include "mqtt_broker/mqtt_types.hpp"

#include <cstddef>
#include <cstdint>

namespace mqtt_broker {

// Generation-stamped slot reference. NULL_PROPERTY indicates exhaustion.
struct PropertyHandle {
    uint16_t slot;
    uint16_t generation;
};

static const PropertyHandle NULL_PROPERTY = {0xFFFF, 0xFFFF};

// Discriminator for decoding a PropertyRecord's blob slice.
enum class PropertyValueKind : uint8_t {
    Byte = 0,
    U16,
    U32,
    Utf8String,
    Binary,
    Varint,
    UserProperty
};

// One parsed property. String/binary values live in the shared blob at offset;
// UserProperty also stores key_offset/key_length for the UTF-8 pair name.
struct PropertyRecord {
    PropertyId id;
    PropertyValueKind kind;
    uint16_t offset;
    uint16_t length;
    uint16_t key_offset;   // UserProperty key in blob; zero otherwise
    uint16_t key_length;
};

struct PropertyPoolConfig {
    size_t max_slots;              // Concurrent property sets (typically per client)
    size_t max_properties_per_slot;
    size_t max_blob_bytes;         // Total blob arena capacity
};

// Single-owner property sets: acquire() / release() with no ref counting.
// Blob bytes are append-only; release() clears the slot but does not reclaim
// blob space — sized for parse-once-per-packet usage across all clients.
class PropertyPool {
public:
    explicit PropertyPool(const PropertyPoolConfig &cfg);
    ~PropertyPool();

    PropertyPool(const PropertyPool &) = delete;
    PropertyPool &operator=(const PropertyPool &) = delete;

    // Returns a fresh handle or NULL_PROPERTY when slots are full.
    PropertyHandle acquire();
    // Invalidates the handle (generation bump); safe on stale handles.
    void release(PropertyHandle h);
    bool valid(PropertyHandle h) const;

    // Each add_* appends wire-encoded value bytes to the blob and records
    // metadata. Returns false on slot/full-blob/record-cap exhaustion.
    bool add_byte(PropertyHandle h, PropertyId id, uint8_t value);
    bool add_u16(PropertyHandle h, PropertyId id, uint16_t value);
    bool add_u32(PropertyHandle h, PropertyId id, uint32_t value);
    bool add_varint(PropertyHandle h, PropertyId id, uint32_t value);
    bool add_utf8(PropertyHandle h, PropertyId id, const char *str, size_t len);
    bool add_binary(PropertyHandle h, PropertyId id, const uint8_t *data, size_t len);
    bool add_user_property(PropertyHandle h, const char *key, size_t key_len, const char *val,
                           size_t val_len);

    size_t count(PropertyHandle h) const;
    // Returns contiguous records for the slot; pointer valid while handle is live.
    const PropertyRecord *records(PropertyHandle h, size_t *out_count) const;
    const uint8_t *blob_data() const { return blob_; }

    size_t bytes_used() const { return bytes_used_; }

private:
    bool append_blob(const uint8_t *data, size_t len, uint16_t *out_offset);
    bool add_record(PropertyHandle h, PropertyId id, PropertyValueKind kind, uint16_t offset,
                    uint16_t length, uint16_t key_offset = 0, uint16_t key_length = 0);

    struct Slot {
        uint16_t generation;
        uint16_t property_count;
        uint16_t first_record;  // Fixed base index: slot * kPropsPerSlot
        bool in_use;
    };

    Slot *slots_;
    size_t slot_cap_;
    PropertyRecord *records_;
    size_t record_cap_;
    uint8_t *blob_;
    size_t blob_cap_;
    size_t bytes_used_;
    uint16_t free_record_head_;  // Reserved; records are slot-partitioned
};

}  // namespace mqtt_broker

#endif
