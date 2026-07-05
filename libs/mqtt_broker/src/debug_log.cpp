// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// debug_log.hpp implementation — optional printf-style broker trace output.

#include "mqtt_broker/debug_log.hpp"

#include <cstdarg>
#include <cstdio>

namespace mqtt_broker {

static BrokerDebugLogFn g_debug_log = nullptr;

void broker_set_debug_log(BrokerDebugLogFn fn)
{
    g_debug_log = fn;
}

const char *reason_code_name(ReasonCode rc)
{
    switch (rc) {
    case ReasonCode::Success:
        return "Success";
    case ReasonCode::GrantedQoS1:
        return "GrantedQoS1";
    case ReasonCode::GrantedQoS2:
        return "GrantedQoS2";
    case ReasonCode::UnspecifiedError:
        return "UnspecifiedError";
    case ReasonCode::MalformedPacket:
        return "MalformedPacket";
    case ReasonCode::ProtocolError:
        return "ProtocolError";
    case ReasonCode::ImplementationSpecificError:
        return "ImplementationSpecificError";
    case ReasonCode::BadUserNameOrPassword:
        return "BadUserNameOrPassword";
    case ReasonCode::NotAuthorized:
        return "NotAuthorized";
    case ReasonCode::TopicNameInvalid:
        return "TopicNameInvalid";
    case ReasonCode::PacketTooLarge:
        return "PacketTooLarge";
    case ReasonCode::QuotaExceeded:
        return "QuotaExceeded";
    case ReasonCode::TopicAliasInvalid:
        return "TopicAliasInvalid";
    case ReasonCode::SubscriptionIdentifiersNotSupported:
        return "SubscriptionIdentifiersNotSupported";
    case ReasonCode::SharedSubscriptionsNotSupported:
        return "SharedSubscriptionsNotSupported";
    case ReasonCode::ServerUnavailable:
        return "ServerUnavailable";
    case ReasonCode::BadAuthenticationMethod:
        return "BadAuthenticationMethod";
    case ReasonCode::SessionTakenOver:
        return "SessionTakenOver";
    default:
        return "Unknown";
    }
}

const char *packet_type_name(PacketType type)
{
    switch (type) {
    case PacketType::Connect:
        return "CONNECT";
    case PacketType::Connack:
        return "CONNACK";
    case PacketType::Publish:
        return "PUBLISH";
    case PacketType::Subscribe:
        return "SUBSCRIBE";
    case PacketType::Unsubscribe:
        return "UNSUBSCRIBE";
    case PacketType::Pingreq:
        return "PINGREQ";
    case PacketType::Disconnect:
        return "DISCONNECT";
    default:
        return "OTHER";
    }
}

void broker_debug_log(const char *fmt, ...)
{
    if (g_debug_log == nullptr || fmt == nullptr) {
        return;
    }

    char line[240];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    line[sizeof(line) - 1] = '\0';
    g_debug_log(line);
}

}  // namespace mqtt_broker
