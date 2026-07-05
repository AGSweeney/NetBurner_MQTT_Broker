// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#include "mqtt_broker/subscription_options.hpp"

#include <cassert>

using namespace mqtt_broker;

static SubscriptionMatch make_match(uint8_t max_qos, uint8_t nl, uint8_t rap, uint8_t rh,
                                    uint16_t session)
{
    SubscriptionMatch m = {};
    m.max_qos = max_qos;
    m.session_id = session;
    m.options.max_qos = max_qos;
    m.options.no_local = nl;
    m.options.retain_as_published = rap;
    m.options.retain_handling = rh;
    return m;
}

static void test_qos_downgrade_overlap()
{
    SubscriptionMatch matches[2] = {
        make_match(0, 0, 0, 0, 10),
        make_match(1, 0, 0, 0, 20),
    };
    DeliveryPlan plan = compute_delivery_plan(2, 99, matches, 2);
    assert(plan.deliver);
    assert(plan.effective_qos == 1);
}

static void test_no_local_eligible_subset()
{
    SubscriptionMatch matches[2] = {
        make_match(2, 1, 0, 0, 5),
        make_match(1, 0, 1, 0, 5),
    };
    DeliveryPlan plan = compute_delivery_plan(2, 5, matches, 2);
    assert(plan.deliver);
    assert(plan.effective_qos == 1);
    assert(plan.retain_as_published);
}

static void test_no_local_full_suppression()
{
    SubscriptionMatch matches[2] = {
        make_match(2, 1, 0, 0, 5),
        make_match(1, 1, 0, 0, 5),
    };
    DeliveryPlan plan = compute_delivery_plan(2, 5, matches, 2);
    assert(!plan.deliver);
}

static void test_subscription_options_validation()
{
    assert(subscription_options_valid(0x00));
    assert(subscription_options_valid(0x10));
    assert(!subscription_options_valid(0xC0));
    assert(!subscription_options_valid(0x30));
}

int main()
{
    test_qos_downgrade_overlap();
    test_no_local_eligible_subset();
    test_no_local_full_suppression();
    test_subscription_options_validation();
    return 0;
}