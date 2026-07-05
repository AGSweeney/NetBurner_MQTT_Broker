// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// MQTT 5 SUBSCRIBE option bits and fan-out delivery planning. Overlapping
// filters for one subscriber session are merged here before enqueue
// (MQTT-5.0 §3.8.3, §3.3.1.3).

#ifndef MQTT_BROKER_SUBSCRIPTION_OPTIONS_HPP
#define MQTT_BROKER_SUBSCRIPTION_OPTIONS_HPP

#include <cstddef>
#include <cstdint>

namespace mqtt_broker {

// Decoded SUBSCRIBE Subscription Options byte (§3.8.3.1).
struct SubscriptionOptions {
    uint8_t max_qos : 2;              // Maximum QoS the server may deliver
    uint8_t no_local : 1;             // 1 => do not deliver self-published messages
    uint8_t retain_as_published : 1;  // 1 => forward incoming RETAIN flag unchanged
    uint8_t retain_handling : 2;      // 0=send retained, 1=send if new, 2=never
    uint8_t reserved : 2;             // Must be 0 on the wire
};

// One matching subscription for a subscriber session during fan-out planning.
struct SubscriptionMatch {
    uint8_t max_qos;           // Granted max QoS for this filter (from SUBACK)
    SubscriptionOptions options;
    uint16_t session_id;       // Subscriber session; compared against publisher for No Local
};

// Resolved delivery decision after merging all matching filters for one subscriber.
struct DeliveryPlan {
    bool deliver;                 // false when every match is No-Local ineligible
    uint8_t effective_qos;        // min(publish_qos, max eligible max_qos) — §3.8.3
    bool retain_as_published;     // true if any eligible match sets RAP
    uint8_t retain_handling;      // minimum RH among eligible matches (most permissive)
};

// Reject reserved bits and Retain Handling value 3 (§3.8.3.1).
bool subscription_options_valid(uint8_t options_byte);

// Merge overlapping subscription matches for one subscriber. publisher_session
// identifies the publishing client for No Local eligibility (eligible-subset
// algorithm, not any-match suppression).
DeliveryPlan compute_delivery_plan(uint8_t publish_qos,
                                   uint16_t publisher_session,
                                   const SubscriptionMatch *matches,
                                   size_t match_count);

}  // namespace mqtt_broker

#endif
