// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef MQTT_BROKER_WIRE_HPP
#define MQTT_BROKER_WIRE_HPP

// Low-level MQTT wire primitives: big-endian integers and UTF-8 strings.
// Multi-byte fields use MSB-first order per MQTT 5 §1.5 (Encoded Data types).
// Safe on little-endian hosts — explicit byte assembly, no native-endian casts.

#include <cstddef>
#include <cstdint>

namespace mqtt_broker {

// Two- and four-byte unsigned integers (§1.5.3). p must point to at least 2/4 bytes.
uint16_t read_u16_be(const uint8_t *p);
uint32_t read_u32_be(const uint8_t *p);
void write_u16_be(uint8_t *p, uint16_t v);
void write_u32_be(uint8_t *p, uint32_t v);

// UTF-8 Encoded String: 2-byte big-endian length prefix + payload (§1.5.4).
// encode returns 0 on overflow/invalid input; decode returns 0 if truncated.
// decode NUL-terminates out (at most out_cap-1 payload bytes copied).
size_t encode_utf8_string(const char *str, uint8_t *out, size_t out_cap);
size_t decode_utf8_string(const uint8_t *in, size_t in_len, char *out, size_t out_cap);

#if defined(MQTT_BROKER_HOST_LE)
// Host-only sanity check for read/write round-trip; no-op on failure.
void wire_self_test();
#endif

}  // namespace mqtt_broker

#endif