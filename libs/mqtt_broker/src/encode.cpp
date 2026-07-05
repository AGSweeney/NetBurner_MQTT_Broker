// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#include "mqtt_broker/encode.hpp"

// MQTT 5 outbound packet builders. PUBLISH uses windowed encoding so the TX
// queue can drain large payloads without a full-packet buffer. Size estimators
// mirror the wire layout for admission checks before enqueue.

#include "mqtt_broker/varint.hpp"
#include "mqtt_broker/wire.hpp"

#include <cstring>

namespace mqtt_broker {

size_t estimate_publish_size(const char *topic, size_t payload_len, bool retain, uint8_t qos,
                             size_t props_len, bool has_expiry)
{
    if (topic == nullptr || qos > 2) {
        return 0;
    }
    size_t tlen = std::strlen(topic);
    size_t vh = 2 + tlen + ((qos > 0) ? 2u : 0u);  // topic len + name [+ packet id]
    size_t prop_total = props_len + (has_expiry ? 5u : 0u);  // 0x02 + 4-byte interval
    uint8_t pl_buf[4];
    size_t pl_size = varint_encode(static_cast<uint32_t>(prop_total), pl_buf, sizeof(pl_buf));
    size_t body = vh + pl_size + prop_total + payload_len;
    uint8_t rl_buf[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(body), rl_buf, sizeof(rl_buf));
    return 1 + rl_size + body;  // fixed header byte + remaining length + body
}

size_t estimate_publish_size(const char *topic, size_t payload_len, bool retain, uint8_t qos)
{
    return estimate_publish_size(topic, payload_len, retain, qos, 0, false);
}

size_t estimate_publish_qos0_size(const char *topic, size_t payload_len, bool retain)
{
    (void)retain;
    return estimate_publish_size(topic, payload_len, retain, 0);
}

size_t serialize_forward_properties(const PropertyPool &pool, PropertyHandle h, uint8_t *out,
                                    size_t cap, uint32_t *expiry_interval)
{
    if (expiry_interval != nullptr) {
        *expiry_interval = 0;
    }
    if (out == nullptr || cap == 0) {
        return 0;
    }
    size_t count = 0;
    const PropertyRecord *recs = pool.records(h, &count);
    if (recs == nullptr) {
        return 0;
    }
    const uint8_t *blob = pool.blob_data();
    size_t off = 0;
    for (size_t i = 0; i < count; ++i) {
        const PropertyRecord &r = recs[i];
        switch (r.id) {
        case MessageExpiryInterval:
            // Broker recomputes remaining expiry at forward time — do not copy verbatim.
            if (r.kind == PropertyValueKind::U32 && expiry_interval != nullptr) {
                *expiry_interval = read_u32_be(blob + r.offset);
            }
            break;
        case PayloadFormatIndicator:
            if (r.kind == PropertyValueKind::Byte && off + 2 <= cap) {
                out[off++] = static_cast<uint8_t>(r.id);
                out[off++] = blob[r.offset];
            }
            break;
        case ContentType:
        case ResponseTopic:
        case CorrelationData:
            if (off + 3 + r.length <= cap) {
                out[off++] = static_cast<uint8_t>(r.id);
                write_u16_be(out + off, r.length);
                off += 2;
                std::memcpy(out + off, blob + r.offset, r.length);
                off += r.length;
            }
            break;
        case UserProperty:
            if (off + 5 + r.key_length + r.length <= cap) {
                out[off++] = static_cast<uint8_t>(r.id);
                write_u16_be(out + off, r.key_length);
                off += 2;
                std::memcpy(out + off, blob + r.key_offset, r.key_length);
                off += r.key_length;
                write_u16_be(out + off, r.length);
                off += 2;
                std::memcpy(out + off, blob + r.offset, r.length);
                off += r.length;
            }
            break;
        default:
            // Not forwardable (topic alias, will delay, CONNECT-only properties, ...).
            break;
        }
    }
    return off;
}

// Shared encoder for PUBACK/PUBREC/PUBREL/PUBCOMP — MQTT-5.0 §3.6.4.
static size_t encode_ack_packet(uint8_t *out, size_t cap, PacketType type, uint16_t packet_id,
                                ReasonCode reason, uint8_t fixed_qos_bits)
{
    if (out == nullptr || cap < 6 || packet_id == 0) {
        return 0;
    }
    size_t remaining = 2 + 1 + 1;  // packet id + reason + property length (0)
    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(remaining), rl, sizeof(rl));
    if (cap < 1 + rl_size + remaining) {
        return 0;
    }
    size_t off = 0;
    out[off++] = packet_type_to_byte(type) | fixed_qos_bits;
    std::memcpy(out + off, rl, rl_size);
    off += rl_size;
    write_u16_be(out + off, packet_id);
    off += 2;
    out[off++] = static_cast<uint8_t>(reason);
    out[off++] = 0x00;  // zero property length
    return off;
}

size_t encode_puback(uint8_t *out, size_t cap, uint16_t packet_id, ReasonCode reason)
{
    return encode_ack_packet(out, cap, PacketType::Puback, packet_id, reason, 0x00u);
}

size_t encode_pubrec(uint8_t *out, size_t cap, uint16_t packet_id, ReasonCode reason)
{
    return encode_ack_packet(out, cap, PacketType::Pubrec, packet_id, reason, 0x00u);
}

size_t encode_pubrel(uint8_t *out, size_t cap, uint16_t packet_id, ReasonCode reason)
{
    return encode_ack_packet(out, cap, PacketType::Pubrel, packet_id, reason, 0x02u);  // QoS 1
}

size_t encode_pubcomp(uint8_t *out, size_t cap, uint16_t packet_id, ReasonCode reason)
{
    return encode_ack_packet(out, cap, PacketType::Pubcomp, packet_id, reason, 0x00u);
}

size_t encode_connack(uint8_t *out, size_t cap, ReasonCode reason, bool session_present,
                      const char *assigned_client_id, uint16_t server_keep_alive)
{
    if (out == nullptr || cap < 5) {
        return 0;
    }
    uint8_t flags = session_present ? 0x01u : 0x00u;
    uint8_t prop_body[160];
    size_t prop_off = 0;
    auto append_byte = [&](uint8_t b) {
        if (prop_off < sizeof(prop_body)) {
            prop_body[prop_off++] = b;
        }
    };
    // Maximum QoS property only permits values 0 or 1; absence means QoS 2 (MQTT-5.0 §3.2.2.3.4).
    if (ConnackCaps::MaximumQos < 2) {
        append_byte(MaximumQoS);
        append_byte(ConnackCaps::MaximumQos);
    }
    append_byte(RetainAvailable);
    append_byte(0x01);
    append_byte(WildcardSubscriptionAvailable);
    append_byte(0x01);
    append_byte(SubscriptionIdentifierAvailable);
    append_byte(0x00);  // broker does not accept subscription identifiers yet
    append_byte(SharedSubscriptionAvailable);
    append_byte(0x00);
    append_byte(TopicAliasMaximum);
    append_byte(0x00);
    append_byte(0x00);
    append_byte(ReceiveMaximum);
    append_byte(static_cast<uint8_t>((ConnackCaps::ReceiveMaximum >> 8) & 0xFFu));
    append_byte(static_cast<uint8_t>(ConnackCaps::ReceiveMaximum & 0xFFu));
    append_byte(MaximumPacketSize);
    append_byte(static_cast<uint8_t>((ConnackCaps::MaximumPacketSize >> 24) & 0xFFu));
    append_byte(static_cast<uint8_t>((ConnackCaps::MaximumPacketSize >> 16) & 0xFFu));
    append_byte(static_cast<uint8_t>((ConnackCaps::MaximumPacketSize >> 8) & 0xFFu));
    append_byte(static_cast<uint8_t>(ConnackCaps::MaximumPacketSize & 0xFFu));
    if (server_keep_alive > 0) {
        append_byte(ServerKeepAlive);
        append_byte(static_cast<uint8_t>((server_keep_alive >> 8) & 0xFFu));
        append_byte(static_cast<uint8_t>(server_keep_alive & 0xFFu));
    }
    if (assigned_client_id != nullptr && assigned_client_id[0] != '\0') {
        size_t id_len = std::strlen(assigned_client_id);
        if (id_len > 0 && id_len < 65535 && prop_off + 3 + id_len <= sizeof(prop_body)) {
            append_byte(AssignedClientIdentifier);
            append_byte(static_cast<uint8_t>((id_len >> 8) & 0xFFu));
            append_byte(static_cast<uint8_t>(id_len & 0xFFu));
            std::memcpy(prop_body + prop_off, assigned_client_id, id_len);
            prop_off += id_len;
        }
    }
    uint8_t prop_len_buf[4];
    size_t prop_len_size =
        varint_encode(static_cast<uint32_t>(prop_off), prop_len_buf, sizeof(prop_len_buf));
    size_t remaining = 2 + prop_len_size + prop_off;  // flags + reason + properties
    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(remaining), rl, sizeof(rl));
    if (cap < 1 + rl_size + remaining) {
        return 0;
    }
    size_t off = 0;
    out[off++] = packet_type_to_byte(PacketType::Connack) | 0x00u;
    std::memcpy(out + off, rl, rl_size);
    off += rl_size;
    out[off++] = flags;
    out[off++] = static_cast<uint8_t>(reason);
    std::memcpy(out + off, prop_len_buf, prop_len_size);
    off += prop_len_size;
    std::memcpy(out + off, prop_body, prop_off);
    off += prop_off;
    return off;
}

size_t encode_disconnect(uint8_t *out, size_t cap, ReasonCode reason)
{
    if (out == nullptr || cap < 4) {
        return 0;
    }
    out[0] = packet_type_to_byte(PacketType::Disconnect) | 0x00u;
    out[1] = 0x02;  // remaining length: reason + zero property length
    out[2] = static_cast<uint8_t>(reason);
    out[3] = 0x00;
    return 4;
}

size_t encode_pingresp(uint8_t *out, size_t cap)
{
    if (out == nullptr || cap < 2) {
        return 0;
    }
    out[0] = packet_type_to_byte(PacketType::Pingresp);
    out[1] = 0x00;
    return 2;
}

size_t encode_suback(uint8_t *out, size_t cap, uint16_t packet_id, const ReasonCode *rcs,
                     size_t rc_count)
{
    if (out == nullptr || rcs == nullptr || rc_count == 0) {
        return 0;
    }
    size_t remaining = 2 + 1 + rc_count;  // packet id + property length (0) + reason list
    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(remaining), rl, sizeof(rl));
    if (cap < 1 + rl_size + remaining) {
        return 0;
    }
    size_t off = 0;
    out[off++] = packet_type_to_byte(PacketType::Suback) | 0x00u;
    std::memcpy(out + off, rl, rl_size);
    off += rl_size;
    write_u16_be(out + off, packet_id);
    off += 2;
    out[off++] = 0x00;
    for (size_t i = 0; i < rc_count; ++i) {
        out[off++] = static_cast<uint8_t>(rcs[i]);
    }
    return off;
}

size_t encode_unsuback(uint8_t *out, size_t cap, uint16_t packet_id, const ReasonCode *rcs,
                       size_t rc_count)
{
    if (out == nullptr || rcs == nullptr || rc_count == 0) {
        return 0;
    }
    size_t remaining = 2 + 1 + rc_count;
    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(remaining), rl, sizeof(rl));
    if (cap < 1 + rl_size + remaining) {
        return 0;
    }
    size_t off = 0;
    out[off++] = packet_type_to_byte(PacketType::Unsuback) | 0x00u;
    std::memcpy(out + off, rl, rl_size);
    off += rl_size;
    write_u16_be(out + off, packet_id);
    off += 2;
    out[off++] = 0x00;
    for (size_t i = 0; i < rc_count; ++i) {
        out[off++] = static_cast<uint8_t>(rcs[i]);
    }
    return off;
}

size_t encode_publish_qos0(uint8_t *out, size_t cap, const char *topic, bool retain,
                           MessagePool *pool, MessageHandle msg, size_t payload_len,
                           size_t encoded_offset)
{
    return encode_publish(out, cap, topic, retain, 0, 0, false, pool, msg, payload_len,
                          encoded_offset);
}

size_t encode_publish(uint8_t *out, size_t cap, const char *topic, bool retain, uint8_t qos,
                      uint16_t packet_id, bool dup, MessagePool *pool, MessageHandle msg,
                      size_t payload_len, size_t encoded_offset, bool has_expiry,
                      uint32_t expiry_remaining)
{
    if (out == nullptr || topic == nullptr || pool == nullptr || qos > 2) {
        return 0;
    }
    if (qos > 0 && packet_id == 0) {
        return 0;
    }
    if (out == nullptr || cap == 0) {
        return 0;
    }
    size_t props_len = 0;
    const uint8_t *props = pool->props(msg, &props_len);
    size_t total = estimate_publish_size(topic, payload_len, retain, qos, props_len, has_expiry);
    if (encoded_offset >= total) {
        return 0;
    }

    // Assemble only the fixed+variable header (bounded, small). The payload is
    // streamed straight from the message pool, so the whole packet is never
    // materialized — this keeps stack use ~600 bytes and lifts the old 2048-byte
    // packet-size ceiling up to the configured MaxPacketBytes.
    size_t tlen = std::strlen(topic);
    size_t vh = 2 + tlen + ((qos > 0) ? 2u : 0u);
    size_t prop_total = props_len + (has_expiry ? 5u : 0u);
    uint8_t pl[4];
    size_t pl_size = varint_encode(static_cast<uint32_t>(prop_total), pl, sizeof(pl));
    size_t body = vh + pl_size + prop_total + payload_len;
    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(body), rl, sizeof(rl));

    // fh(1) + rl(<=4) + topiclen(2) + topic(<=256) + pid(2) + proplen(<=4) +
    // expiry(5) + forwarded props(<=kMessagePropsCapacity).
    uint8_t header[16 + 256 + kMessagePropsCapacity];
    size_t hlen = 0;
    uint8_t fh = packet_type_to_byte(PacketType::Publish);
    if (retain) {
        fh |= 0x01u;
    }
    fh |= static_cast<uint8_t>((qos & 0x03u) << 1);
    if (dup) {
        fh |= 0x08u;
    }
    header[hlen++] = fh;
    std::memcpy(header + hlen, rl, rl_size);
    hlen += rl_size;
    write_u16_be(header + hlen, static_cast<uint16_t>(tlen));
    hlen += 2;
    std::memcpy(header + hlen, topic, tlen);
    hlen += tlen;
    if (qos > 0) {
        write_u16_be(header + hlen, packet_id);
        hlen += 2;
    }
    std::memcpy(header + hlen, pl, pl_size);
    hlen += pl_size;
    if (has_expiry) {
        header[hlen++] = MessageExpiryInterval;
        write_u32_be(header + hlen, expiry_remaining);
        hlen += 4;
    }
    if (props_len > 0 && props != nullptr) {
        std::memcpy(header + hlen, props, props_len);
        hlen += props_len;
    }

    // Emit the window [encoded_offset, encoded_offset + cap) intersected with the
    // full packet [0, total), copying from the header region and/or the streamed
    // payload region as needed.
    size_t produced = 0;
    size_t pos = encoded_offset;
    size_t end = encoded_offset + cap;
    if (end > total) {
        end = total;
    }
    while (pos < end) {
        if (pos < hlen) {
            size_t chunk = (end < hlen ? end : hlen) - pos;
            std::memcpy(out + produced, header + pos, chunk);
            produced += chunk;
            pos += chunk;
        } else {
            size_t payload_pos = pos - hlen;
            size_t copied =
                pool->read_payload(msg, payload_pos, out + produced, end - pos);
            if (copied == 0) {
                break;
            }
            produced += copied;
            pos += copied;
        }
    }
    return produced;
}

}  // namespace mqtt_broker
