#ifndef MQTT_BROKER_TYPES_HPP
#define MQTT_BROKER_TYPES_HPP

// Generic broker handle types shared across modules.
// MessageHandle is a generational slot index into MessagePool — not a wire type.

#include <cstdint>

namespace mqtt_broker {

// Opaque reference to a pooled message buffer; generation detects stale reuse.
struct MessageHandle {
    uint16_t slot;
    uint16_t generation;
};

static const MessageHandle NULL_MESSAGE = {0xFFFF, 0xFFFF};

// False for NULL_MESSAGE; true for any other slot/generation pair.
inline bool message_handle_valid(MessageHandle h)
{
    return h.slot != 0xFFFF || h.generation != 0xFFFF;
}

inline bool message_handle_equal(MessageHandle a, MessageHandle b)
{
    return a.slot == b.slot && a.generation == b.generation;
}

}  // namespace mqtt_broker

#endif