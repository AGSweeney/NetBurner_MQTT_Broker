// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// MQTT 5 property validation (§2.2): allowed IDs per packet type, UTF-8 well-formedness,
// single-occurrence rules, and wire parsing into PropertyPool. Returns ok=false with a
// ReasonCode suitable for CONNACK/SUBACK/DISCONNECT (§2.4).

#ifndef MQTT_BROKER_PROPERTY_RULES_HPP
#define MQTT_BROKER_PROPERTY_RULES_HPP

#include "mqtt_broker/mqtt_types.hpp"
#include "mqtt_broker/property_pool.hpp"

namespace mqtt_broker {

// Result of validate_properties / parse_and_validate_properties.
struct PropertyValidationResult {
    bool ok;
    ReasonCode reason;
};

// Returns true if data[0..len) is well-formed UTF-8 (valid ranges, no overlong sequences).
bool utf8_valid(const uint8_t *data, size_t len);

// NUL-terminated string variant of utf8_valid.
bool utf8_cstr_valid(const char *str);

// Validates an already-parsed property set against packet-type allowlists (§2.2.2),
// duplicate single-occurrence IDs, and value constraints (Receive Maximum != 0,
// Topic Alias == 0 on PUBLISH). Invalid or empty handle returns ok=true.
PropertyValidationResult validate_properties(PacketType packet, PropertyHandle props,
                                             const PropertyPool &pool);

// Decodes the property length and property list from wire bytes into pool, then runs
// validate_properties. On failure releases any acquired handle; on success sets
// *out_handle. MalformedPacket for wire errors; QuotaExceeded if pool is full.
PropertyValidationResult parse_and_validate_properties(PacketType packet, const uint8_t *data,
                                                       size_t len, PropertyPool *pool,
                                                       PropertyHandle *out_handle);

}  // namespace mqtt_broker

#endif
