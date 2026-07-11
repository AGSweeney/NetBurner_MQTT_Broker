// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef MQTT_BROKER_MQTT_TYPES_HPP
#define MQTT_BROKER_MQTT_TYPES_HPP

// MQTT 5 wire-level enumerations: packet types, property IDs, reason codes.
// PacketType maps to the fixed-header control packet type nibble (§2.1.1).

#include <cstdint>

namespace mqtt_broker {

// Control packet types 1–15 (§2.1.3); value 0 is reserved on the wire.
enum class PacketType : uint8_t {
    Reserved = 0,
    Connect = 1,
    Connack = 2,
    Publish = 3,
    Puback = 4,
    Pubrec = 5,
    Pubrel = 6,
    Pubcomp = 7,
    Subscribe = 8,
    Suback = 9,
    Unsubscribe = 10,
    Unsuback = 11,
    Pingreq = 12,
    Pingresp = 13,
    Disconnect = 14,
    Auth = 15
};

// Property identifiers in CONNECT/PUBLISH/ACK property sections (§2.2.2).
enum PropertyId : uint8_t {
    PayloadFormatIndicator = 0x01,
    MessageExpiryInterval = 0x02,
    ContentType = 0x03,
    ResponseTopic = 0x08,
    CorrelationData = 0x09,
    SubscriptionIdentifier = 0x0B,
    SessionExpiryInterval = 0x11,
    AssignedClientIdentifier = 0x12,
    ServerKeepAlive = 0x13,
    AuthenticationMethod = 0x15,
    AuthenticationData = 0x16,
    RequestProblemInformation = 0x17,
    WillDelayInterval = 0x18,
    RequestResponseInformation = 0x19,
    ResponseInformation = 0x1A,
    ServerReference = 0x1C,
    ReasonString = 0x1F,
    ReceiveMaximum = 0x21,
    TopicAliasMaximum = 0x22,
    TopicAlias = 0x23,
    MaximumQoS = 0x24,
    RetainAvailable = 0x25,
    UserProperty = 0x26,
    MaximumPacketSize = 0x27,
    WildcardSubscriptionAvailable = 0x28,
    SubscriptionIdentifierAvailable = 0x29,
    SharedSubscriptionAvailable = 0x2A
};

// Reason codes for CONNACK, PUBACK, DISCONNECT, and other ack packets (§2.4).
enum ReasonCode : uint8_t {
    Success = 0x00,
    GrantedQoS1 = 0x01,
    GrantedQoS2 = 0x02,
    NoSubscriptionExisted = 0x11,
    UnspecifiedError = 0x80,
    MalformedPacket = 0x81,
    ProtocolError = 0x82,
    ImplementationSpecificError = 0x83,
    UnsupportedProtocolVersion = 0x84,
    ClientIdentifierNotValid = 0x85,
    BadUserNameOrPassword = 0x86,
    NotAuthorized = 0x87,
    TopicNameInvalid = 0x90,
    PacketTooLarge = 0x95,
    QuotaExceeded = 0x97,
    TopicAliasInvalid = 0x94,
    SubscriptionIdentifiersNotSupported = 0xA1,
    SharedSubscriptionsNotSupported = 0x9E,
    ServerUnavailable = 0x88,
    BadAuthenticationMethod = 0x8C,
    SessionTakenOver = 0x8E
};

// Extract/set the MQTT control packet type in the fixed-header byte (§2.1.1).
inline PacketType packet_type_from_byte(uint8_t b)
{
    return static_cast<PacketType>((b >> 4) & 0x0Fu);
}

inline uint8_t packet_type_to_byte(PacketType t)
{
    return static_cast<uint8_t>(t) << 4;
}

}  // namespace mqtt_broker

#endif