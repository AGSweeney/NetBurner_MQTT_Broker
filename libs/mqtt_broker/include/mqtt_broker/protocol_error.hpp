// Maps ReasonCode outcomes from validation and parsing to broker actions: whether to
// disconnect the client and whether to publish the will message (§3.14.2).

#ifndef MQTT_BROKER_PROTOCOL_ERROR_HPP
#define MQTT_BROKER_PROTOCOL_ERROR_HPP

#include "mqtt_broker/mqtt_types.hpp"

namespace mqtt_broker {

// Disconnect/publish_will flags refine how the broker responds to a given reason.
struct ProtocolErrorAction {
    ReasonCode reason;
    bool disconnect;
    bool publish_will;
};

// Returns default {reason, disconnect=false, publish_will=false}. Fatal wire/protocol
// errors set both flags; capability rejections (subscription ids, shared subs) do not.
ProtocolErrorAction protocol_error_action(ReasonCode reason);

}  // namespace mqtt_broker

#endif
