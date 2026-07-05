// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef MQTT_BROKER_CONNACK_CAPS_HPP
#define MQTT_BROKER_CONNACK_CAPS_HPP

// Static CONNACK capabilities advertised to connecting clients (§3.2.3).
// Values must match actual broker behavior and BrokerLimits resource tables.

#include "mqtt_broker/broker_limits.hpp"

#include <cstdint>

namespace mqtt_broker {

// Compile-time defaults encoded as CONNACK properties when a client connects.
struct ConnackCaps {
    static constexpr uint8_t MaximumQos = 2;  // §3.2.2.3.2 — broker accepts QoS 0–2
    static constexpr bool AssignedClientIdentifierAvailable = true;
    static constexpr bool RetainAvailable = true;
    static constexpr bool WildcardSubscriptionAvailable = true;
    static constexpr bool SubscriptionIdentifierAvailable = false;
    static constexpr bool SharedSubscriptionAvailable = false;
    static constexpr uint16_t TopicAliasMaximum = 0;  // Topic aliases not supported
    // Must not exceed per-client QoS 2 inflight table (Receive Maximum property).
    static constexpr uint16_t ReceiveMaximum =
        static_cast<uint16_t>(BrokerLimits::QosInflightPerClient);
    // Broker inbound ceiling advertised to clients (§3.2.2.3.5).
    static constexpr uint32_t MaximumPacketSize =
        static_cast<uint32_t>(BrokerLimits::MaxPacketBytes);
};

}  // namespace mqtt_broker

#endif