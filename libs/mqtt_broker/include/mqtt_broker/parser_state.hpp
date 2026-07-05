#ifndef MQTT_BROKER_PARSER_STATE_HPP
#define MQTT_BROKER_PARSER_STATE_HPP

// Incremental packet parser FSM phases and resource limits.
// ParsePhase drives PacketParser state; see parser.hpp for the full machine.

#include <cstdint>

namespace mqtt_broker {

enum class ParsePhase : uint8_t {
    Idle = 0,           // Awaiting first byte of next packet
    FixedHeader,        // Reading byte 1 (type + flags)
    RemainingLength,    // Variable-byte remaining length (§1.5.5)
    VariableHeader,     // Type-specific header before property section
    Properties,         // MQTT 5 property length + property bytes (§2.2)
    Payload,            // Remaining bytes after variable header + properties
    Complete,           // One full packet decoded; read via PacketParser::packet()
    Error               // Parse failed; read PacketParser::error_reason()
};

// Hard caps enforced during incremental parse; must fit control_buf_ in parser.
struct ParserLimits {
    uint32_t max_packet_bytes;      // Reject when remaining length exceeds this
    uint16_t max_topic_bytes;       // Max UTF-8 topic/filter string length
    uint16_t control_buffer_bytes;  // Max variable-header staging size
};

}  // namespace mqtt_broker

#endif