#ifndef MQTT_BROKER_PARSER_HPP
#define MQTT_BROKER_PARSER_HPP

// Incremental MQTT 5 wire parser (FSM). Feed arbitrary byte chunks via feed();
// when packet_ready() is true, read the decoded fields from packet(). Large
// PUBLISH payloads stream directly into the message pool without a full-packet
// copy. On failure, error_reason() holds the CONNACK/DISCONNECT reason to send.

#include "mqtt_broker/message_pool.hpp"
#include "mqtt_broker/mqtt_types.hpp"
#include "mqtt_broker/parser_state.hpp"
#include "mqtt_broker/property_pool.hpp"
#include "mqtt_broker/types.hpp"

#include <cstddef>
#include <cstdint>

namespace mqtt_broker {

static const size_t kMaxSubscribeEntries = 8;
static const size_t kMaxUsernameLen = 64;   // incl. NUL terminator
static const size_t kMaxPasswordLen = 64;

struct ParsedSubscribeEntry {
    char filter[256];
    uint8_t options_byte;
};

// Decoded view of one complete packet. Only fields relevant to packet_.type are
// populated; others are zero/default. Payload/properties are pool handles — the
// broker must release them when done (or transfer ownership downstream).
struct ParsedPacket {
    PacketType type;
    uint8_t flags;
    uint32_t remaining_length;
    char topic[256];
    uint16_t packet_id;         // QoS > 0 publish and all ack packets
    uint8_t ack_reason_code;    // PUBACK/PUBREC/PUBREL/PUBCOMP/SUBACK/UNSUBACK entries
    uint8_t qos;
    bool retain;
    bool dup;
    MessageHandle payload;
    PropertyHandle properties;
    size_t payload_len;

    char client_id[256];
    char protocol_name[16];
    uint8_t protocol_level;
    uint8_t connect_flags;
    uint16_t keep_alive;
    bool clean_start;
    uint32_t client_max_packet_size;
    uint16_t client_receive_maximum;
    uint32_t session_expiry_interval;
    uint32_t will_delay_interval;
    uint32_t disconnect_session_expiry;
    uint8_t disconnect_reason;

    // CONNECT credentials. Oversized values set the has_* flag but leave the
    // buffer empty, so they can never match a stored user.
    bool has_username;
    bool has_password;
    char username[kMaxUsernameLen];
    uint8_t password[kMaxPasswordLen];
    size_t password_len;

    bool will_flag;
    uint8_t will_qos;
    bool will_retain;
    char will_topic[256];
    MessageHandle will_payload;
    size_t will_payload_len;
    PropertyHandle will_properties;

    ParsedSubscribeEntry subscribe_entries[kMaxSubscribeEntries];
    size_t subscribe_count;
};

class PacketParser {
public:
    PacketParser(const ParserLimits &limits, MessagePool *msg_pool, PropertyPool *prop_pool);

    PacketParser(const PacketParser &) = delete;
    PacketParser &operator=(const PacketParser &) = delete;

    // Consume up to len bytes; returns count consumed. Call reset() before parsing
    // the next packet after packet_ready() or a parse error.
    size_t feed(const uint8_t *data, size_t len);
    void reset();

    ParsePhase phase() const { return phase_; }
    bool packet_ready() const { return phase_ == ParsePhase::Complete; }
    ReasonCode error_reason() const { return error_reason_; }
    const ParsedPacket &packet() const { return packet_; }

private:
    size_t consume_byte(uint8_t b);
    size_t consume_buffer(const uint8_t *data, size_t len);
    bool enter_error(ReasonCode reason);
    bool finish_packet();
    bool parse_variable_header_byte(uint8_t b);
    bool parse_properties_byte(uint8_t b);
    bool parse_payload_byte(uint8_t b);

    ParserLimits limits_;
    MessagePool *msg_pool_;
    PropertyPool *prop_pool_;
    ParsePhase phase_;
    ReasonCode error_reason_;

    uint8_t fixed_header_;
    uint32_t remaining_total_;
    uint32_t remaining_consumed_;
    uint8_t rl_buf_[4];
    size_t rl_buf_len_;

    uint8_t control_buf_[1024];
    size_t control_len_;
    size_t control_target_;

    uint32_t property_section_len_;
    uint32_t property_section_consumed_;
    bool property_len_known_;

    ParsedPacket packet_;
    MessageHandle active_payload_;
};

}  // namespace mqtt_broker

#endif