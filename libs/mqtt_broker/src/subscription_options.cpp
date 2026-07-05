// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// SUBSCRIBE option validation and per-subscriber delivery plan merging.

#include "mqtt_broker/subscription_options.hpp"

#include <algorithm>

namespace mqtt_broker {

bool subscription_options_valid(uint8_t options_byte)
{
    uint8_t rh = (options_byte >> 4) & 0x03u;
    uint8_t reserved = (options_byte >> 6) & 0x03u;
    return reserved == 0 && rh <= 2;  // RH=3 is a Malformed Packet (§3.8.3.1)
}

DeliveryPlan compute_delivery_plan(uint8_t publish_qos,
                                   uint16_t publisher_session,
                                   const SubscriptionMatch *matches,
                                   size_t match_count)
{
    DeliveryPlan plan = {false, 0, false, 0};

    if (matches == nullptr || match_count == 0) {
        return plan;
    }

    uint8_t max_eligible_qos = 0;
    bool any_eligible = false;
    bool any_rap = false;
    uint8_t rh_min = 3;  // Lower RH value is more permissive (0 beats 1 beats 2)

    for (size_t i = 0; i < match_count; ++i) {
        const SubscriptionMatch &m = matches[i];
        // No Local excludes only same-session matches; other filters still count.
        bool ineligible = (m.options.no_local != 0) && (m.session_id == publisher_session);
        if (ineligible) {
            continue;
        }

        any_eligible = true;
        max_eligible_qos = std::max(max_eligible_qos, m.max_qos);
        if (m.options.retain_as_published != 0) {
            any_rap = true;
        }
        uint8_t rh = m.options.retain_handling;
        if (rh < rh_min) {
            rh_min = rh;
        }
    }

    if (!any_eligible) {
        return plan;
    }

    plan.deliver = true;
    plan.effective_qos = std::min(publish_qos, max_eligible_qos);
    plan.retain_as_published = any_rap;
    plan.retain_handling = rh_min;
    return plan;
}

}  // namespace mqtt_broker
