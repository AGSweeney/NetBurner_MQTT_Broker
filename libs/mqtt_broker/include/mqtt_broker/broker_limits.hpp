// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef MQTT_BROKER_LIMITS_HPP
#define MQTT_BROKER_LIMITS_HPP

// Compile-time capacity profile selector. Picks one BrokerLimits struct for the
// target platform (or MQTT_BROKER_HOST_LE for desktop acceptance tests). All broker
// subsystems read limits through BrokerLimits:: constants — never hard-code sizes.

#if defined(MQTT_BROKER_HOST_LE)
#include "mqtt_broker/broker_limits_host.hpp"
#elif defined(PLATFORM_NANO54415) || defined(NANO54415) || defined(PLATFORM_MOD5441X) || defined(MOD5441X)
#include "mqtt_broker/broker_limits_nano54415.hpp"
#elif defined(PLATFORM_MODM7AE70) || defined(MODM7AE70)
#include "mqtt_broker/broker_limits_modm7ae70.hpp"
#elif defined(PLATFORM_SOMRT1061) || defined(SOMRT1061) || defined(PLATFORM_MODRT1171) || defined(MODRT1171)
#include "mqtt_broker/broker_limits_somrt1061.hpp"
#else
#error "No broker limits profile selected. Define PLATFORM or MQTT_BROKER_HOST_LE."
#endif

#endif
