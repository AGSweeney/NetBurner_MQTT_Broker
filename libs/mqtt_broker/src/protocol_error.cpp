// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// ReasonCode → disconnect/will policy for parser and packet handlers.
// Graceful client DISCONNECT will policy (reason 0x04 only) lives in broker.cpp.

#include "mqtt_broker/protocol_error.hpp"

namespace mqtt_broker {

ProtocolErrorAction protocol_error_action(ReasonCode reason)
{
    ProtocolErrorAction action = {reason, false, false};

    switch (reason) {
    // §3.14.2: protocol violations close the connection and publish the will (if any).
    case ReasonCode::MalformedPacket:
    case ReasonCode::ProtocolError:
    case ReasonCode::TopicNameInvalid:
    case ReasonCode::TopicAliasInvalid:
    case ReasonCode::PacketTooLarge:
        action.disconnect = true;
        action.publish_will = true;
        break;
    // Capability rejections — respond with reason on the ack packet, stay connected.
    case ReasonCode::SubscriptionIdentifiersNotSupported:
    case ReasonCode::SharedSubscriptionsNotSupported:
        action.disconnect = false;
        action.publish_will = false;
        break;
    default:
        break;
    }

    return action;
}

}  // namespace mqtt_broker
