// MQTT 5 property parsing and validation — wire decode into PropertyPool plus
// post-parse rule checks (allowlist, duplicates, UTF-8, numeric constraints).

#include "mqtt_broker/property_rules.hpp"

#include "mqtt_broker/varint.hpp"
#include "mqtt_broker/wire.hpp"

#include <cstring>

namespace mqtt_broker {

// Broker cap on property-section length; oversized Property Length → ProtocolError.
static const uint32_t kMaxPropertyBytes = 2048;

bool utf8_valid(const uint8_t *data, size_t len)
{
    if (data == nullptr) {
        return false;
    }
    size_t i = 0;
    while (i < len) {
        uint8_t c = data[i];
        if (c <= 0x7Fu) {
            i++;
            continue;
        }
        if (c >= 0xC2u && c <= 0xDFu) {
            if (i + 1 >= len || (data[i + 1] & 0xC0u) != 0x80u) {
                return false;
            }
            i += 2;
            continue;
        }
        if (c >= 0xE0u && c <= 0xEFu) {
            if (i + 2 >= len || (data[i + 1] & 0xC0u) != 0x80u || (data[i + 2] & 0xC0u) != 0x80u) {
                return false;
            }
            i += 3;
            continue;
        }
        if (c >= 0xF0u && c <= 0xF4u) {
            if (i + 3 >= len || (data[i + 1] & 0xC0u) != 0x80u || (data[i + 2] & 0xC0u) != 0x80u ||
                (data[i + 3] & 0xC0u) != 0x80u) {
                return false;
            }
            i += 4;
            continue;
        }
        return false;
    }
    return true;
}

bool utf8_cstr_valid(const char *str)
{
    if (str == nullptr) {
        return false;
    }
    return utf8_valid(reinterpret_cast<const uint8_t *>(str), std::strlen(str));
}

// §2.2.2 — which Property IDs are legal on each packet type.
static bool property_allowed(PacketType packet, PropertyId id)
{
    switch (packet) {
    case PacketType::Connect:
        switch (id) {
        case SessionExpiryInterval:
        case ReceiveMaximum:
        case MaximumPacketSize:
        case TopicAliasMaximum:
        case RequestResponseInformation:
        case RequestProblemInformation:
        case UserProperty:
        case AuthenticationMethod:
        case AuthenticationData:
        case WillDelayInterval:
        case PayloadFormatIndicator:
        case MessageExpiryInterval:
        case ContentType:
        case ResponseTopic:
        case CorrelationData:
            return true;
        default:
            return false;
        }
    case PacketType::Publish:
        switch (id) {
        case PayloadFormatIndicator:
        case MessageExpiryInterval:
        case ContentType:
        case ResponseTopic:
        case CorrelationData:
        case UserProperty:
            return true;
        case TopicAlias:
            return true;
        default:
            return false;
        }
    case PacketType::Subscribe:
        switch (id) {
        case UserProperty:
            return true;
        // §3.8.3.1 — broker does not support Subscription Identifier on SUBSCRIBE.
        case SubscriptionIdentifier:
            return false;
        default:
            return false;
        }
    case PacketType::Connack:
    case PacketType::Puback:
    case PacketType::Pubrec:
    case PacketType::Pubrel:
    case PacketType::Pubcomp:
        return id == UserProperty || id == ReasonString;
    case PacketType::Disconnect:
        // §3.14.2.2: Session Expiry Interval, Reason String, User Property.
        return id == UserProperty || id == ReasonString || id == SessionExpiryInterval;
    default:
        return false;
    }
}

// §2.2.2.2 — these IDs may appear at most once per property list.
static bool single_occurrence(PropertyId id)
{
    switch (id) {
    case SessionExpiryInterval:
    case ReceiveMaximum:
    case MaximumPacketSize:
    case TopicAliasMaximum:
    case RequestResponseInformation:
    case RequestProblemInformation:
    case AuthenticationMethod:
    case AuthenticationData:
    case WillDelayInterval:
    case PayloadFormatIndicator:
    case MessageExpiryInterval:
    case ContentType:
    case ResponseTopic:
    case CorrelationData:
    case TopicAlias:
        return true;
    default:
        return false;
    }
}

// §2.4 — disallowed property maps to the reason the client should receive.
static ReasonCode reject_reason(PacketType packet, PropertyId id)
{
    if (packet == PacketType::Subscribe && id == SubscriptionIdentifier) {
        return ReasonCode::SubscriptionIdentifiersNotSupported;
    }
    if (id == TopicAlias) {
        return ReasonCode::TopicAliasInvalid;
    }
    return ReasonCode::ProtocolError;
}

PropertyValidationResult validate_properties(PacketType packet, PropertyHandle props,
                                             const PropertyPool &pool)
{
    PropertyValidationResult result = {true, ReasonCode::Success};
    if (!pool.valid(props)) {
        return result;
    }

    size_t count = 0;
    const PropertyRecord *recs = pool.records(props, &count);
    bool seen[256] = {};

    for (size_t i = 0; i < count; ++i) {
        const PropertyRecord &r = recs[i];
        uint8_t id_byte = static_cast<uint8_t>(r.id);

        if (!property_allowed(packet, r.id)) {
            result.ok = false;
            result.reason = reject_reason(packet, r.id);
            return result;
        }

        if (single_occurrence(r.id)) {
            if (seen[id_byte]) {
                result.ok = false;
                result.reason = ReasonCode::ProtocolError;
                return result;
            }
            seen[id_byte] = true;
        }

        const uint8_t *blob = pool.blob_data();
        if (r.kind == PropertyValueKind::Utf8String) {
            if (!utf8_valid(blob + r.offset, r.length)) {
                result.ok = false;
                result.reason = ReasonCode::ProtocolError;
                return result;
            }
        } else if (r.kind == PropertyValueKind::UserProperty) {
            if (!utf8_valid(blob + r.key_offset, r.key_length) ||
                !utf8_valid(blob + r.offset, r.length)) {
                result.ok = false;
                result.reason = ReasonCode::ProtocolError;
                return result;
            }
        } else if (r.id == ReceiveMaximum && r.kind == PropertyValueKind::U16) {
            // §3.1.2.11.2 — Receive Maximum MUST NOT be 0.
            uint16_t v = read_u16_be(blob + r.offset);
            if (v == 0) {
                result.ok = false;
                result.reason = ReasonCode::ProtocolError;
                return result;
            }
        } else if (r.id == TopicAlias && r.kind == PropertyValueKind::U16) {
            // §3.3.2.3.4 — Topic Alias on PUBLISH must be 0 (broker does not accept aliases).
            uint16_t alias = read_u16_be(blob + r.offset);
            if (alias != 0) {
                result.ok = false;
                result.reason = ReasonCode::TopicAliasInvalid;
                return result;
            }
        }
    }

    return result;
}

PropertyValidationResult parse_and_validate_properties(PacketType packet, const uint8_t *data,
                                                       size_t len, PropertyPool *pool,
                                                       PropertyHandle *out_handle)
{
    PropertyValidationResult result = {true, ReasonCode::Success};
    if (pool == nullptr || out_handle == nullptr) {
        result.ok = false;
        result.reason = ReasonCode::ImplementationSpecificError;
        return result;
    }

    *out_handle = NULL_PROPERTY;
    if (len == 0) {
        return result;
    }

    VarintDecode prop_len = varint_decode(data, len);
    if (prop_len.result != VarintResult::Ok) {
        result.ok = false;
        result.reason = ReasonCode::MalformedPacket;
        return result;
    }
    if (prop_len.value > kMaxPropertyBytes) {
        result.ok = false;
        result.reason = ReasonCode::ProtocolError;
        return result;
    }
    if (prop_len.bytes_consumed + prop_len.value > len) {
        result.ok = false;
        result.reason = ReasonCode::MalformedPacket;
        return result;
    }

    PropertyHandle h = pool->acquire();
    if (!pool->valid(h)) {
        result.ok = false;
        result.reason = ReasonCode::QuotaExceeded;
        return result;
    }

    const uint8_t *cursor = data + prop_len.bytes_consumed;
    size_t remaining = prop_len.value;

    while (remaining > 0) {
        VarintDecode id_dec = varint_decode(cursor, remaining);
        if (id_dec.result != VarintResult::Ok) {
            pool->release(h);
            result.ok = false;
            result.reason = ReasonCode::MalformedPacket;
            return result;
        }
        cursor += id_dec.bytes_consumed;
        remaining -= id_dec.bytes_consumed;

        PropertyId pid = static_cast<PropertyId>(id_dec.value);
        if (!property_allowed(packet, pid)) {
            pool->release(h);
            result.ok = false;
            result.reason = reject_reason(packet, pid);
            return result;
        }

        bool stored = false;
        switch (pid) {
        case PayloadFormatIndicator:
        case RequestProblemInformation:
        case RequestResponseInformation:
            if (remaining < 1) {
                pool->release(h);
                result.ok = false;
                result.reason = ReasonCode::MalformedPacket;
                return result;
            }
            stored = pool->add_byte(h, pid, cursor[0]);
            cursor++;
            remaining--;
            break;
        case ReceiveMaximum:
        case TopicAliasMaximum:
        case TopicAlias:
            if (remaining < 2) {
                pool->release(h);
                result.ok = false;
                result.reason = ReasonCode::MalformedPacket;
                return result;
            }
            stored = pool->add_u16(h, pid, read_u16_be(cursor));
            cursor += 2;
            remaining -= 2;
            break;
        case SessionExpiryInterval:
        case MessageExpiryInterval:
        case WillDelayInterval:
        case MaximumPacketSize:
            if (remaining < 4) {
                pool->release(h);
                result.ok = false;
                result.reason = ReasonCode::MalformedPacket;
                return result;
            }
            stored = pool->add_u32(h, pid, read_u32_be(cursor));
            cursor += 4;
            remaining -= 4;
            break;
        case SubscriptionIdentifier: {
            VarintDecode sid = varint_decode(cursor, remaining);
            if (sid.result != VarintResult::Ok) {
                pool->release(h);
                result.ok = false;
                result.reason = ReasonCode::MalformedPacket;
                return result;
            }
            stored = pool->add_varint(h, pid, sid.value);
            cursor += sid.bytes_consumed;
            remaining -= sid.bytes_consumed;
            break;
        }
        case ContentType:
        case ResponseTopic:
        case AuthenticationMethod:
        case ReasonString: {
            if (remaining < 2) {
                pool->release(h);
                result.ok = false;
                result.reason = ReasonCode::MalformedPacket;
                return result;
            }
            uint16_t slen = read_u16_be(cursor);
            if (remaining < static_cast<size_t>(2 + slen)) {
                pool->release(h);
                result.ok = false;
                result.reason = ReasonCode::MalformedPacket;
                return result;
            }
            if (!utf8_valid(cursor + 2, slen)) {
                pool->release(h);
                result.ok = false;
                result.reason = ReasonCode::ProtocolError;
                return result;
            }
            stored = pool->add_utf8(h, pid, reinterpret_cast<const char *>(cursor + 2), slen);
            cursor += 2 + slen;
            remaining -= 2 + slen;
            break;
        }
        case CorrelationData:
        case AuthenticationData: {
            if (remaining < 2) {
                pool->release(h);
                result.ok = false;
                result.reason = ReasonCode::MalformedPacket;
                return result;
            }
            uint16_t blen = read_u16_be(cursor);
            if (remaining < static_cast<size_t>(2 + blen)) {
                pool->release(h);
                result.ok = false;
                result.reason = ReasonCode::MalformedPacket;
                return result;
            }
            stored = pool->add_binary(h, pid, cursor + 2, blen);
            cursor += 2 + blen;
            remaining -= 2 + blen;
            break;
        }
        case UserProperty: {
            if (remaining < 2) {
                pool->release(h);
                result.ok = false;
                result.reason = ReasonCode::MalformedPacket;
                return result;
            }
            uint16_t klen = read_u16_be(cursor);
            if (remaining < static_cast<size_t>(2 + klen + 2)) {
                pool->release(h);
                result.ok = false;
                result.reason = ReasonCode::MalformedPacket;
                return result;
            }
            uint16_t vlen = read_u16_be(cursor + 2 + klen);
            if (remaining < static_cast<size_t>(4 + klen + vlen)) {
                pool->release(h);
                result.ok = false;
                result.reason = ReasonCode::MalformedPacket;
                return result;
            }
            const char *key = reinterpret_cast<const char *>(cursor + 2);
            const char *val = reinterpret_cast<const char *>(cursor + 4 + klen);
            if (!utf8_valid(reinterpret_cast<const uint8_t *>(key), klen) ||
                !utf8_valid(reinterpret_cast<const uint8_t *>(val), vlen)) {
                pool->release(h);
                result.ok = false;
                result.reason = ReasonCode::ProtocolError;
                return result;
            }
            stored = pool->add_user_property(h, key, klen, val, vlen);
            cursor += 4 + klen + vlen;
            remaining -= 4 + klen + vlen;
            break;
        }
        default:
            pool->release(h);
            result.ok = false;
            result.reason = ReasonCode::ProtocolError;
            return result;
        }

        if (!stored) {
            pool->release(h);
            result.ok = false;
            result.reason = ReasonCode::QuotaExceeded;
            return result;
        }
    }

    // Duplicate and value-constraint checks after full list is parsed.
    result = validate_properties(packet, h, *pool);
    if (!result.ok) {
        pool->release(h);
        return result;
    }

    *out_handle = h;
    return result;
}

}  // namespace mqtt_broker
