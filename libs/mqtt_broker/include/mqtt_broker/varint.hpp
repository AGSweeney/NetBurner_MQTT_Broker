#ifndef MQTT_BROKER_VARINT_HPP
#define MQTT_BROKER_VARINT_HPP

// MQTT Variable Byte Integer encode/decode (§1.5.5).
// Used for remaining length and property-length fields; max 4 bytes, max 268435455.

#include <cstddef>
#include <cstdint>

namespace mqtt_broker {

enum class VarintResult {
    Ok,         // Complete value in bytes_consumed
    NeedMore,   // Input truncated before continuation bit cleared
    TooLarge,   // More than 4 bytes without terminator
    Overflow    // Decoded value exceeds 268435455
};

struct VarintDecode {
    VarintResult result;
    uint32_t value;          // Valid only when result == Ok
    size_t bytes_consumed;   // Bytes read from data (may be partial on error)
};

// Decode up to len bytes; does not read past a successful terminator.
VarintDecode varint_decode(const uint8_t *data, size_t len);
// Encode value; returns byte count or 0 if out_cap too small or value > max.
size_t varint_encode(uint32_t value, uint8_t *out, size_t out_cap);
// Encoded length without writing (1–4 for valid MQTT values).
size_t varint_encoded_size(uint32_t value);

}  // namespace mqtt_broker

#endif