// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef BROKER_CONFIG_HPP
#define BROKER_CONFIG_HPP

// NetBurner app-config bridge: reads listener ports, TLS/plain toggles, session
// timing, and auth policy from persisted config_server settings and pushes them
// into the portable mqtt_broker library.

// Copies session timing limits into mqtt_broker::broker_set_policy(), clamped to compile-time caps.
void BrokerConfigApplyPolicy();
bool BrokerConfigPlainTcpEnabled();
int BrokerConfigPlainTcpPort();
bool BrokerConfigTlsEnabled();
int BrokerConfigTlsPort();
bool BrokerConfigAuthRequired();
bool BrokerConfigAllowAnonymous();
// Updates auth flags in app config and persists to storage.
void BrokerConfigSetAuth(bool auth_required, bool allow_anonymous);

#endif
