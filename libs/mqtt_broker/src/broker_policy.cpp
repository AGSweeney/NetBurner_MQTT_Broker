// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// Process-wide broker policy and optional CONNECT auth hook. Defaults come from
// BrokerLimits; platforms may override via broker_set_policy() / broker_set_auth_handler().

#include "mqtt_broker/broker_policy.hpp"
#include "mqtt_broker/broker_limits.hpp"

namespace mqtt_broker {

static BrokerPolicy gPolicy = {
    BrokerLimits::MaxKeepAliveSec,
    BrokerLimits::ConnectHandshakeTimeoutSec,
};

const BrokerPolicy &broker_policy()
{
    return gPolicy;
}

void broker_set_policy(const BrokerPolicy &policy)
{
    gPolicy = policy;
}

static AuthCheckFn gAuthHandler = nullptr;

void broker_set_auth_handler(AuthCheckFn fn)
{
    gAuthHandler = fn;
}

AuthCheckFn broker_auth_handler()
{
    return gAuthHandler;
}

}  // namespace mqtt_broker
