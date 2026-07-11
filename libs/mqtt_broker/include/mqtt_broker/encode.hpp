// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef MQTT_BROKER_ENCODE_HPP
#define MQTT_BROKER_ENCODE_HPP

// Outbound MQTT 5 packet encoding. All encode_* functions write wire bytes
// into out[0..cap) and return bytes written, or 0 on insufficient cap or
// invalid arguments. PUBLISH encoding supports chunked output via encoded_offset.

#include "mqtt_broker/connack_caps.hpp"
#include "mqtt_broker/message_pool.hpp"
#include "mqtt_broker/mqtt_types.hpp"
#include "mqtt_broker/property_pool.hpp"
#include "mqtt_broker/topic_intern.hpp"

#include <cstddef>
#include <cstdint>

namespace mqtt_broker {

// Returns total on-wire size for a QoS 0 PUBLISH (fixed header through payload).
// protocol_level 4 (MQTT 3.1.1) omits the property section entirely.
size_t estimate_publish_qos0_size(const char *topic, size_t payload_len, bool retain);
size_t estimate_publish_size(const char *topic, size_t payload_len, bool retain, uint8_t qos);
size_t estimate_publish_size(const char *topic, size_t payload_len, bool retain, uint8_t qos,
                             size_t props_len, bool has_expiry, uint8_t protocol_level = 5);

// Re-serializes the forwardable PUBLISH properties (payload format indicator, content
// type, response topic, correlation data, user properties) into wire form. Message
// Expiry Interval is extracted into *expiry_interval instead of being serialized.
size_t serialize_forward_properties(const PropertyPool &pool, PropertyHandle h, uint8_t *out,
                                    size_t cap, uint32_t *expiry_interval);

// CONNACK. Level 5 carries broker capability properties from ConnackCaps
// (MQTT-5.0 §3.2); level 4 is the 2-byte MQTT 3.1.1 form with the ReasonCode
// mapped to the closest 3.1.1 return code (§3.2.2.3 of MQTT-3.1.1).
size_t encode_connack(uint8_t *out, size_t cap, ReasonCode reason, bool session_present,
                      const char *assigned_client_id, uint16_t server_keep_alive = 0,
                      uint8_t protocol_level = 5);
// MQTT-5.0 §3.14 — DISCONNECT with reason code only (no properties). MQTT 3.1.1
// has no server-to-client DISCONNECT; callers must skip it for level 4.
size_t encode_disconnect(uint8_t *out, size_t cap, ReasonCode reason);
// MQTT-5.0 §3.12.2 — PINGRESP (zero remaining length).
size_t encode_pingresp(uint8_t *out, size_t cap);
// SUBACK reason/return-code list (one byte per subscription). Level 4 omits the
// property length byte and maps error reasons to 0x80.
size_t encode_suback(uint8_t *out, size_t cap, uint16_t packet_id, const ReasonCode *rcs,
                     size_t rc_count, uint8_t protocol_level = 5);
// UNSUBACK. Level 4 is packet id only (MQTT-3.1.1 §3.11); level 5 appends reasons.
size_t encode_unsuback(uint8_t *out, size_t cap, uint16_t packet_id, const ReasonCode *rcs,
                       size_t rc_count, uint8_t protocol_level = 5);
// QoS 0 PUBLISH; encoded_offset selects a slice for incremental TX drain.
size_t encode_publish_qos0(uint8_t *out, size_t cap, const char *topic, bool retain,
                           MessagePool *pool, MessageHandle msg, size_t payload_len,
                           size_t encoded_offset);
// QoS 0–2 PUBLISH. Payload is streamed from pool; header assembled on stack.
// Level 4 omits the property section (and message expiry) from the wire format.
size_t encode_publish(uint8_t *out, size_t cap, const char *topic, bool retain, uint8_t qos,
                    uint16_t packet_id, bool dup, MessagePool *pool, MessageHandle msg,
                    size_t payload_len, size_t encoded_offset, bool has_expiry = false,
                    uint32_t expiry_remaining = 0, uint8_t protocol_level = 5);
// QoS 1/2 acknowledgement packets. Level 5 appends reason + zero property length
// (MQTT-5.0 §3.6.4); level 4 is packet id only.
size_t encode_puback(uint8_t *out, size_t cap, uint16_t packet_id, ReasonCode reason,
                     uint8_t protocol_level = 5);
size_t encode_pubrec(uint8_t *out, size_t cap, uint16_t packet_id, ReasonCode reason,
                     uint8_t protocol_level = 5);
size_t encode_pubrel(uint8_t *out, size_t cap, uint16_t packet_id, ReasonCode reason,
                     uint8_t protocol_level = 5);
size_t encode_pubcomp(uint8_t *out, size_t cap, uint16_t packet_id, ReasonCode reason,
                      uint8_t protocol_level = 5);

}  // namespace mqtt_broker

#endif
