// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#include "mqtt_broker/parser.hpp"

#include "mqtt_broker/connack_caps.hpp"
#include "mqtt_broker/property_rules.hpp"
#include "mqtt_broker/topic_trie.hpp"
#include "mqtt_broker/varint.hpp"
#include "mqtt_broker/wire.hpp"

#include <cstring>

// PacketParser incremental FSM — one byte at a time via consume_byte():
//
//   Idle/FixedHeader → first byte selects packet type and flags
//   RemainingLength  → MQTT variable-byte integer; enforces max packet size
//   VariableHeader   → type-specific VH (topic, packet id, CONNECT fields, …)
//   Properties       → property-length prefix + validated section (MQTT 5 §2.2)
//   Payload          → remaining bytes (PUBLISH app payload or CONNECT/SUB body)
//   Complete / Error → packet ready for broker dispatch, or parse failure
//
// feed() loops until the input chunk is consumed or a phase needs more data.

namespace mqtt_broker {

PacketParser::PacketParser(const ParserLimits &limits, MessagePool *msg_pool,
                           PropertyPool *prop_pool)
    : limits_(limits),
      msg_pool_(msg_pool),
      prop_pool_(prop_pool),
      phase_(ParsePhase::Idle),
      error_reason_(ReasonCode::Success),
      fixed_header_(0),
      remaining_total_(0),
      remaining_consumed_(0),
      rl_buf_len_(0),
      control_len_(0),
      control_target_(0),
      property_section_len_(0),
      property_section_consumed_(0),
      property_len_known_(false),
      active_payload_(NULL_MESSAGE)
{
    if (limits_.control_buffer_bytes > sizeof(control_buf_)) {
        limits_.control_buffer_bytes = sizeof(control_buf_);
    }
    reset();
}

void PacketParser::reset()
{
    phase_ = ParsePhase::Idle;
    error_reason_ = ReasonCode::Success;
    fixed_header_ = 0;
    remaining_total_ = 0;
    remaining_consumed_ = 0;
    rl_buf_len_ = 0;
    control_len_ = 0;
    control_target_ = 0;
    property_section_len_ = 0;
    property_section_consumed_ = 0;
    property_len_known_ = false;
    active_payload_ = NULL_MESSAGE;

    packet_ = {};
    packet_.payload = NULL_MESSAGE;
    packet_.properties = NULL_PROPERTY;
    packet_.will_payload = NULL_MESSAGE;
    packet_.will_properties = NULL_PROPERTY;
}

bool PacketParser::enter_error(ReasonCode reason)
{
    phase_ = ParsePhase::Error;
    error_reason_ = reason;
    if (msg_pool_ != nullptr && message_handle_valid(active_payload_)) {
        msg_pool_->msg_release(active_payload_);
        active_payload_ = NULL_MESSAGE;
    }
    if (prop_pool_ != nullptr && prop_pool_->valid(packet_.properties)) {
        prop_pool_->release(packet_.properties);
        packet_.properties = NULL_PROPERTY;
    }
    if (prop_pool_ != nullptr && prop_pool_->valid(packet_.will_properties)) {
        prop_pool_->release(packet_.will_properties);
        packet_.will_properties = NULL_PROPERTY;
    }
    if (msg_pool_ != nullptr && msg_pool_->msg_valid(packet_.will_payload)) {
        msg_pool_->msg_release(packet_.will_payload);
        packet_.will_payload = NULL_MESSAGE;
    }
    return false;
}

static bool parse_connect_variable_header(const uint8_t *buf, size_t len, ParsedPacket *pkt,
                                          uint16_t max_topic)
{
    if (pkt == nullptr || len < 6) {
        return false;
    }
    uint16_t plen = read_u16_be(buf);
    if (plen == 0 || static_cast<size_t>(2 + plen + 4) > len || plen >= sizeof(pkt->protocol_name)) {
        return false;
    }
    std::memcpy(pkt->protocol_name, buf + 2, plen);
    pkt->protocol_name[plen] = '\0';
    pkt->protocol_level = buf[2 + plen];
    pkt->connect_flags = buf[2 + plen + 1];
    pkt->keep_alive = read_u16_be(buf + 2 + plen + 2);
    pkt->clean_start = (pkt->connect_flags & 0x02u) != 0;
    (void)max_topic;
    return std::strcmp(pkt->protocol_name, "MQTT") == 0 && pkt->protocol_level == 5;
}

static bool parse_connect_will_section(const uint8_t *buf, size_t len, size_t *off,
                                       ParsedPacket *pkt, MessagePool *pool,
                                       PropertyPool *props)
{
    if (pkt == nullptr || off == nullptr) {
        return false;
    }
    if ((pkt->connect_flags & 0x04u) == 0) {
        return true;
    }
    if (*off > len) {
        return false;
    }

    pkt->will_flag = true;
    pkt->will_qos = static_cast<uint8_t>((pkt->connect_flags >> 3) & 0x03u);
    pkt->will_retain = (pkt->connect_flags & 0x20u) != 0;

    VarintDecode prop_len = varint_decode(buf + *off, len - *off);
    if (prop_len.result != VarintResult::Ok) {
        return false;
    }
    size_t will_props_end = *off + prop_len.bytes_consumed + prop_len.value;
    if (will_props_end > len) {
        return false;
    }
    if (props != nullptr && prop_len.value > 0) {
        // Will properties reuse the CONNECT allow-list (payload format, expiry,
        // content type, response topic, correlation data, will delay, user props).
        PropertyValidationResult vr = parse_and_validate_properties(
            PacketType::Connect, buf + *off, prop_len.bytes_consumed + prop_len.value, props,
            &pkt->will_properties);
        if (!vr.ok) {
            return false;
        }
    }
    *off = will_props_end;

    if (*off + 2 > len) {
        return false;
    }
    uint16_t topic_len = read_u16_be(buf + *off);
    *off += 2;
    if (*off + topic_len > len || topic_len == 0 || topic_len >= sizeof(pkt->will_topic)) {
        return false;
    }
    std::memcpy(pkt->will_topic, buf + *off, topic_len);
    pkt->will_topic[topic_len] = '\0';
    *off += topic_len;
    if (!TopicTrie::topic_valid(pkt->will_topic)) {
        return false;
    }

    if (*off + 2 > len) {
        return false;
    }
    uint16_t payload_len = read_u16_be(buf + *off);
    *off += 2;
    if (*off + payload_len > len) {
        return false;
    }
    if (pool != nullptr && payload_len > 0) {
        pkt->will_payload = pool->acquire_empty();
        if (!message_handle_valid(pkt->will_payload)) {
            return false;
        }
        if (!pool->append_payload(pkt->will_payload, buf + *off, payload_len)) {
            pool->msg_release(pkt->will_payload);
            pkt->will_payload = NULL_MESSAGE;
            return false;
        }
    }
    pkt->will_payload_len = payload_len;
    *off += payload_len;
    return true;
}

static bool parse_connect_payload(const uint8_t *buf, size_t len, ParsedPacket *pkt,
                                  MessagePool *pool, PropertyPool *props)
{
    if (pkt == nullptr || len < 2) {
        return false;
    }
    pkt->will_flag = false;
    pkt->will_qos = 0;
    pkt->will_retain = false;
    pkt->will_topic[0] = '\0';
    pkt->will_payload = NULL_MESSAGE;
    pkt->will_payload_len = 0;
    pkt->will_properties = NULL_PROPERTY;
    pkt->has_username = false;
    pkt->has_password = false;
    pkt->username[0] = '\0';
    pkt->password_len = 0;

    uint16_t cid_len = read_u16_be(buf);
    if (static_cast<size_t>(2 + cid_len) > len || cid_len >= sizeof(pkt->client_id)) {
        return false;
    }
    if (cid_len > 0) {
        std::memcpy(pkt->client_id, buf + 2, cid_len);
    }
    pkt->client_id[cid_len] = '\0';

    size_t off = 2 + cid_len;
    if (!parse_connect_will_section(buf, len, &off, pkt, pool, props)) {
        return false;
    }

    if ((pkt->connect_flags & 0x80u) != 0) {
        if (off + 2 > len) {
            return false;
        }
        uint16_t ulen = read_u16_be(buf + off);
        off += 2;
        if (off + ulen > len) {
            return false;
        }
        pkt->has_username = true;
        if (ulen < sizeof(pkt->username)) {
            std::memcpy(pkt->username, buf + off, ulen);
            pkt->username[ulen] = '\0';
        }
        off += ulen;
    }
    if ((pkt->connect_flags & 0x40u) != 0) {
        if (off + 2 > len) {
            return false;
        }
        uint16_t plen = read_u16_be(buf + off);
        off += 2;
        if (off + plen > len) {
            return false;
        }
        pkt->has_password = true;
        if (plen <= sizeof(pkt->password)) {
            std::memcpy(pkt->password, buf + off, plen);
            pkt->password_len = plen;
        }
        off += plen;
    }
    return off == len;
}

static bool parse_subscribe_payload(const uint8_t *buf, size_t len, ParsedPacket *pkt)
{
    if (pkt == nullptr) {
        return false;
    }
    pkt->subscribe_count = 0;
    size_t off = 0;
    while (off + 3 <= len && pkt->subscribe_count < kMaxSubscribeEntries) {
        uint16_t flen = read_u16_be(buf + off);
        off += 2;
        if (off + flen + 1 > len || flen >= sizeof(pkt->subscribe_entries[0].filter)) {
            return false;
        }
        std::memcpy(pkt->subscribe_entries[pkt->subscribe_count].filter, buf + off, flen);
        pkt->subscribe_entries[pkt->subscribe_count].filter[flen] = '\0';
        off += flen;
        pkt->subscribe_entries[pkt->subscribe_count].options_byte = buf[off++];
        pkt->subscribe_count++;
    }
    return off == len && pkt->subscribe_count > 0;
}

static bool parse_unsubscribe_payload(const uint8_t *buf, size_t len, ParsedPacket *pkt)
{
    if (pkt == nullptr) {
        return false;
    }
    pkt->subscribe_count = 0;
    size_t off = 0;
    while (off + 2 <= len && pkt->subscribe_count < kMaxSubscribeEntries) {
        uint16_t flen = read_u16_be(buf + off);
        off += 2;
        if (off + flen > len || flen >= sizeof(pkt->subscribe_entries[0].filter)) {
            return false;
        }
        std::memcpy(pkt->subscribe_entries[pkt->subscribe_count].filter, buf + off, flen);
        pkt->subscribe_entries[pkt->subscribe_count].filter[flen] = '\0';
        pkt->subscribe_entries[pkt->subscribe_count].options_byte = 0;
        off += flen;
        pkt->subscribe_count++;
    }
    return off == len && pkt->subscribe_count > 0;
}

static void extract_connect_limits(ParsedPacket *pkt, PropertyPool *pool)
{
    pkt->client_max_packet_size = ConnackCaps::MaximumPacketSize;
    pkt->client_receive_maximum = static_cast<uint16_t>(ConnackCaps::ReceiveMaximum);
    pkt->session_expiry_interval = 0;
    pkt->will_delay_interval = 0;
    if (pool == nullptr || !pool->valid(pkt->properties)) {
        return;
    }
    size_t count = 0;
    const PropertyRecord *recs = pool->records(pkt->properties, &count);
    const uint8_t *blob = pool->blob_data();
    for (size_t i = 0; i < count; ++i) {
        if (recs[i].id == MaximumPacketSize && recs[i].kind == PropertyValueKind::U32) {
            pkt->client_max_packet_size = read_u32_be(blob + recs[i].offset);
        } else if (recs[i].id == ReceiveMaximum && recs[i].kind == PropertyValueKind::U16) {
            pkt->client_receive_maximum = read_u16_be(blob + recs[i].offset);
        } else if (recs[i].id == SessionExpiryInterval && recs[i].kind == PropertyValueKind::U32) {
            pkt->session_expiry_interval = read_u32_be(blob + recs[i].offset);
        } else if (recs[i].id == WillDelayInterval && recs[i].kind == PropertyValueKind::U32) {
            pkt->will_delay_interval = read_u32_be(blob + recs[i].offset);
        }
    }
}

// Will Delay Interval lives in the will-property section (MQTT 5 §3.1.3.2.2); the
// CONNECT-property fallback in extract_connect_limits is kept for tolerance.
static void extract_will_delay(ParsedPacket *pkt, PropertyPool *pool)
{
    if (pool == nullptr || !pool->valid(pkt->will_properties)) {
        return;
    }
    size_t count = 0;
    const PropertyRecord *recs = pool->records(pkt->will_properties, &count);
    const uint8_t *blob = pool->blob_data();
    for (size_t i = 0; i < count; ++i) {
        if (recs[i].id == WillDelayInterval && recs[i].kind == PropertyValueKind::U32) {
            pkt->will_delay_interval = read_u32_be(blob + recs[i].offset);
        }
    }
}

static void extract_disconnect_session_expiry(ParsedPacket *pkt, PropertyPool *pool)
{
    pkt->disconnect_session_expiry = 0xFFFFFFFFu;
    if (pool == nullptr || !pool->valid(pkt->properties)) {
        return;
    }
    size_t count = 0;
    const PropertyRecord *recs = pool->records(pkt->properties, &count);
    const uint8_t *blob = pool->blob_data();
    for (size_t i = 0; i < count; ++i) {
        if (recs[i].id == SessionExpiryInterval && recs[i].kind == PropertyValueKind::U32) {
            pkt->disconnect_session_expiry = read_u32_be(blob + recs[i].offset);
        }
    }
}

bool PacketParser::finish_packet()
{
    if (packet_.type == PacketType::Publish && msg_pool_ != nullptr &&
        message_handle_valid(active_payload_)) {
        packet_.payload = active_payload_;
        packet_.payload_len = msg_pool_->payload_size(active_payload_);
    } else if (packet_.type == PacketType::Connect) {
        if (!parse_connect_payload(control_buf_, control_len_, &packet_, msg_pool_, prop_pool_)) {
            return enter_error(ReasonCode::ProtocolError);
        }
        extract_connect_limits(&packet_, prop_pool_);
        extract_will_delay(&packet_, prop_pool_);
    } else if (packet_.type == PacketType::Disconnect) {
        extract_disconnect_session_expiry(&packet_, prop_pool_);
    } else if (packet_.type == PacketType::Subscribe) {
        if (!parse_subscribe_payload(control_buf_, control_len_, &packet_)) {
            return enter_error(ReasonCode::MalformedPacket);
        }
    } else if (packet_.type == PacketType::Unsubscribe) {
        if (!parse_unsubscribe_payload(control_buf_, control_len_, &packet_)) {
            return enter_error(ReasonCode::MalformedPacket);
        }
    }
    phase_ = ParsePhase::Complete;
    return true;
}

size_t PacketParser::feed(const uint8_t *data, size_t len)
{
    if (data == nullptr || len == 0) {
        return 0;
    }

    size_t consumed = 0;
    while (consumed < len) {
        if (phase_ == ParsePhase::Complete || phase_ == ParsePhase::Error) {
            break;
        }
        size_t n = consume_byte(data[consumed]);
        if (n == 0) {
            break;
        }
        consumed += n;
    }
    return consumed;
}

size_t PacketParser::consume_byte(uint8_t b)
{
    switch (phase_) {
    case ParsePhase::Idle:
    case ParsePhase::FixedHeader:
        fixed_header_ = b;
        packet_.type = packet_type_from_byte(b);
        packet_.flags = static_cast<uint8_t>(b & 0x0Fu);
        packet_.qos = static_cast<uint8_t>((b >> 1) & 0x03u);
        packet_.dup = (b & 0x08u) != 0;
        packet_.retain = (b & 0x01u) != 0;
        packet_.disconnect_reason = 0;  // RL=0 DISCONNECT implies Normal (0x00)
        rl_buf_len_ = 0;
        phase_ = ParsePhase::RemainingLength;
        return 1;

    case ParsePhase::RemainingLength:
        rl_buf_[rl_buf_len_++] = b;
        if (rl_buf_len_ > 4) {
            enter_error(ReasonCode::MalformedPacket);
            return 1;
        }
        {
            VarintDecode d = varint_decode(rl_buf_, rl_buf_len_);
            if (d.result == VarintResult::NeedMore) {
                return 1;
            }
            if (d.result != VarintResult::Ok) {
                enter_error(ReasonCode::MalformedPacket);
                return 1;
            }
            remaining_total_ = d.value;
            packet_.remaining_length = d.value;
            remaining_consumed_ = 0;
            control_len_ = 0;
            property_len_known_ = false;
            property_section_len_ = 0;
            property_section_consumed_ = 0;

            if (remaining_total_ > limits_.max_packet_bytes) {
                enter_error(ReasonCode::PacketTooLarge);
                return 1;
            }

            if (remaining_total_ == 0) {
                finish_packet();
                return 1;
            }

            if (packet_.type == PacketType::Publish) {
                if (msg_pool_ != nullptr) {
                    active_payload_ = msg_pool_->acquire_empty();
                    if (!message_handle_valid(active_payload_)) {
                        enter_error(ReasonCode::QuotaExceeded);
                        return 1;
                    }
                }
                control_target_ = 2;
                phase_ = ParsePhase::VariableHeader;
            } else if (packet_.type == PacketType::Subscribe) {
                control_target_ = 2;
                phase_ = ParsePhase::VariableHeader;
            } else if (packet_.type == PacketType::Connect) {
                control_target_ = 2;
                phase_ = ParsePhase::VariableHeader;
            } else if (packet_.type == PacketType::Unsubscribe) {
                control_target_ = 2;
                phase_ = ParsePhase::VariableHeader;
            } else if (packet_.type == PacketType::Puback || packet_.type == PacketType::Pubrec ||
                       packet_.type == PacketType::Pubrel || packet_.type == PacketType::Pubcomp) {
                control_target_ = 2;
                phase_ = ParsePhase::VariableHeader;
            } else if (packet_.type == PacketType::Disconnect) {
                control_target_ = 1;
                phase_ = ParsePhase::VariableHeader;
            } else {
                phase_ = ParsePhase::Payload;
            }
        }
        return 1;

    case ParsePhase::VariableHeader:
        if (control_len_ >= sizeof(control_buf_)) {
            enter_error(ReasonCode::MalformedPacket);
            return 1;
        }
        control_buf_[control_len_++] = b;
        remaining_consumed_++;

        if (packet_.type == PacketType::Publish) {
            if (control_len_ == 2) {
                uint16_t tlen = read_u16_be(control_buf_);
                if (tlen == 0 || tlen > limits_.max_topic_bytes) {
                    enter_error(ReasonCode::TopicNameInvalid);
                    return 1;
                }
                control_target_ = 2 + tlen + ((packet_.qos > 0) ? 2u : 0u);
            }
            if (control_len_ >= control_target_) {
                uint16_t tlen = read_u16_be(control_buf_);
                if (control_len_ < static_cast<size_t>(2 + tlen)) {
                    return 1;
                }
                if (tlen >= sizeof(packet_.topic)) {
                    enter_error(ReasonCode::TopicNameInvalid);
                    return 1;
                }
                std::memcpy(packet_.topic, control_buf_ + 2, tlen);
                packet_.topic[tlen] = '\0';
                if (!TopicTrie::topic_valid(packet_.topic)) {
                    enter_error(ReasonCode::TopicNameInvalid);
                    return 1;
                }
                size_t off = 2 + tlen;
                if (packet_.qos > 0) {
                    packet_.packet_id = read_u16_be(control_buf_ + off);
                    if (packet_.packet_id == 0) {
                        enter_error(ReasonCode::ProtocolError);
                        return 1;
                    }
                } else if (packet_.qos == 0 && control_len_ >= control_target_) {
                    packet_.packet_id = 0;
                }
                control_len_ = 0;
                phase_ = ParsePhase::Properties;
                property_len_known_ = false;
                property_section_len_ = 0;
            }
        } else if (packet_.type == PacketType::Subscribe) {
            if (control_len_ >= 2) {
                packet_.packet_id = read_u16_be(control_buf_);
                control_len_ = 0;
                phase_ = ParsePhase::Properties;
                property_len_known_ = false;
                property_section_len_ = 0;
            }
        } else if (packet_.type == PacketType::Connect) {
            if (control_len_ == 2) {
                uint16_t plen = read_u16_be(control_buf_);
                if (plen == 0 || plen > 8) {
                    enter_error(ReasonCode::ProtocolError);
                    return 1;
                }
                control_target_ = 2 + plen + 4;
            }
            if (control_len_ >= control_target_ && control_target_ > 2) {
                if (!parse_connect_variable_header(control_buf_, control_len_, &packet_,
                                                   limits_.max_topic_bytes)) {
                    enter_error(ReasonCode::ProtocolError);
                    return 1;
                }
                control_len_ = 0;
                phase_ = ParsePhase::Properties;
                property_len_known_ = false;
                property_section_len_ = 0;
            }
        } else if (packet_.type == PacketType::Unsubscribe) {
            if (control_len_ >= 2) {
                packet_.packet_id = read_u16_be(control_buf_);
                control_len_ = 0;
                phase_ = ParsePhase::Properties;
                property_len_known_ = false;
                property_section_len_ = 0;
            }
        } else if (packet_.type == PacketType::Puback || packet_.type == PacketType::Pubrec ||
                   packet_.type == PacketType::Pubrel || packet_.type == PacketType::Pubcomp) {
            // Variable header: packet id, then optional reason code, then optional properties.
            // Remaining length 2 (id only) and 3 (id + reason) are both legal (MQTT 5 §3.4.2).
            if (control_len_ == 2) {
                packet_.packet_id = read_u16_be(control_buf_);
                packet_.ack_reason_code = 0x00;
                if (remaining_consumed_ >= remaining_total_) {
                    finish_packet();
                    return 1;
                }
                control_target_ = 3;
            } else if (control_len_ >= 3) {
                packet_.ack_reason_code = control_buf_[2];
                if (remaining_consumed_ >= remaining_total_) {
                    finish_packet();
                    return 1;
                }
                control_len_ = 0;
                phase_ = ParsePhase::Properties;
                property_len_known_ = false;
                property_section_len_ = 0;
            }
        } else if (packet_.type == PacketType::Disconnect) {
            // Variable header: reason code, then optional properties (MQTT 5 §3.14.2).
            packet_.disconnect_reason = control_buf_[0];
            if (remaining_consumed_ >= remaining_total_) {
                finish_packet();
                return 1;
            }
            control_len_ = 0;
            phase_ = ParsePhase::Properties;
            property_len_known_ = false;
            property_section_len_ = 0;
        } else {
            if (remaining_consumed_ >= remaining_total_) {
                finish_packet();
            } else {
                phase_ = ParsePhase::Properties;
            }
        }
        return 1;

    case ParsePhase::Properties:
        if (control_len_ >= sizeof(control_buf_)) {
            enter_error(ReasonCode::MalformedPacket);
            return 1;
        }
        control_buf_[control_len_++] = b;
        remaining_consumed_++;

        if (!property_len_known_) {
            VarintDecode d = varint_decode(control_buf_, control_len_);
            if (d.result == VarintResult::NeedMore) {
                return 1;
            }
            if (d.result != VarintResult::Ok) {
                enter_error(ReasonCode::MalformedPacket);
                return 1;
            }
            property_section_len_ = d.value;
            property_len_known_ = true;
            control_target_ = d.bytes_consumed + property_section_len_;
        }

        if (control_len_ < control_target_) {
            return 1;
        }

        {
            PropertyValidationResult vr = {true, ReasonCode::Success};
            if (prop_pool_ != nullptr) {
                vr = parse_and_validate_properties(packet_.type, control_buf_, control_len_,
                                                   prop_pool_, &packet_.properties);
            }
            if (!vr.ok) {
                enter_error(vr.reason);
                return 1;
            }
            size_t after_props = remaining_total_ - remaining_consumed_;
            if (after_props == 0) {
                finish_packet();
            } else {
                control_len_ = 0;
                phase_ = ParsePhase::Payload;
            }
        }
        return 1;

    case ParsePhase::Payload:
        remaining_consumed_++;
        if (packet_.type == PacketType::Publish && msg_pool_ != nullptr &&
            message_handle_valid(active_payload_)) {
            if (!msg_pool_->append_payload(active_payload_, &b, 1)) {
                enter_error(ReasonCode::QuotaExceeded);
                return 1;
            }
        } else if (packet_.type == PacketType::Connect || packet_.type == PacketType::Subscribe ||
                   packet_.type == PacketType::Unsubscribe) {
            if (control_len_ < sizeof(control_buf_)) {
                control_buf_[control_len_++] = b;
            } else {
                enter_error(ReasonCode::MalformedPacket);
                return 1;
            }
        }
        if (remaining_consumed_ >= remaining_total_) {
            finish_packet();
        }
        return 1;

    default:
        return 0;
    }
}

}  // namespace mqtt_broker