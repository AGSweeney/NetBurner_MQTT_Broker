# Broker Scope and Feature Notes

This note describes the intended scope of the NetBurner MQTT Broker: what it provides today, which MQTT capabilities are deliberately out of scope for an embedded target, and areas that may be extended in future releases. It is meant as a planning reference, not a checklist of deficiencies.

The broker targets industrial and edge use cases—Sparkplug B, telemetry fan-out, and local device integration—on resource-constrained NetBurner hardware. Design choices favor predictable memory use, a single-threaded core, and honest CONNACK advertisement of supported features.

## Core capabilities

The following are fully supported and covered by host tests and on-device conformance runs:

- **MQTT 3.1.1 and MQTT 5.0** on the same TCP/MQTTS listeners
- **QoS 0, 1, and 2** with the complete QoS 2 four-step handshake
- **Publish/subscribe routing**, including wildcard filters (`+`, `#`)
- **Retained messages** (set, deliver, and delete via zero-length payload)
- **Last Will and Testament**, including Will Delay and DISCONNECT-with-Will (MQTT 5)
- **Session persistence** (clean start / session present) and a bounded offline queue for QoS 1 and 2
- **Keep-alive enforcement** and connect handshake timeout
- **CONNECT authentication** (optional username/password; up to 16 users on EFFS)
- **Cross-version delivery** (MQTT 5 publishers to MQTT 3.1.1 subscribers, with properties stripped as appropriate)
- **MQTT 5 delivery properties**: session expiry, message expiry, subscription options (No Local, Retain-as-Published, Retain Handling), user properties, and related PUBLISH metadata
- **Outbound flow control** via Receive Maximum and per-client Maximum Packet Size
- **Web admin UI**, REST status API, and optional Let's Encrypt TLS (SOMRT1061)

CONNACK capabilities are defined in `libs/mqtt_broker/include/mqtt_broker/connack_caps.hpp` and summarized in the README **Broker Capabilities** section.

## Optional MQTT 5 features not included

Several MQTT 5 features are optional in the specification and are intentionally omitted on embedded targets. Clients that request them receive a clear rejection or see the capability advertised as unavailable in CONNACK:

| Feature | Approach |
|---------|----------|
| Shared subscriptions (`$share/...`) | Not supported; SUBSCRIBE returns `SharedSubscriptionsNotSupported` |
| Topic aliases | Not supported; `TopicAliasMaximum = 0` |
| Subscription identifiers | Not supported; SUBSCRIBE returns `SubscriptionIdentifiersNotSupported` |
| Enhanced authentication (AUTH exchange) | Not supported; AUTH packets are rejected |
| Server redirect (`ServerReference`) | Not implemented |
| Request/response (`ResponseInformation` in CONNACK) | CONNECT property accepted; no response semantics |

These omissions are by design. Most Sparkplug and IIoT clients do not depend on them. CONNACK properties accurately reflect broker behavior so clients can adapt at connect time.

## Authorization model

Authentication verifies identity at **CONNECT** time. When enabled, the broker checks username and password (salted SHA-256 on flash) and rejects failed attempts with the appropriate CONNACK reason code.

**Topic-level access control (ACL)**—rules that restrict which clients may publish or subscribe to which topics—is not part of the current release. After a successful CONNECT, an authenticated client may use any valid topic name or filter. ACL is a natural candidate for a future enhancement where deployments require per-user or per-role topic boundaries.

## Durability and persistence

Retained messages, active sessions, and offline queues live in **RAM**. They provide full MQTT semantics while the device is running but do not survive a reboot or firmware update. Credential storage and broker configuration are persisted on EFFS; broker message state is not.

This matches typical embedded broker expectations: flash wear and RAM budget are reserved for configuration and application data rather than a full message store. Deployments that need retained traffic to survive power cycles should plan around an upstream broker or accept re-publication after restart.

## Behavioral notes

A few specification-permitted behaviors are implemented with embedded-friendly limits:

- **Offline delivery** applies to QoS 1 and 2 only. QoS 0 is not queued for offline sessions (per MQTT semantics).
- **Offline queue depth** is capped at 16 messages per client (see platform limit headers).
- **Inbound Maximum Packet Size** is governed by the compile-time `MaxPacketBytes` ceiling rather than per-client inbound negotiation.
- **Reason strings** on ack packets are not emitted; numeric reason codes are used throughout.

Platform capacity tables (clients, payload pool, TX queues, retained store, TLS slots) are documented in `broker_limits_*.hpp` and the README **Platform Limits** section. These are resource ceilings, not incomplete protocol handlers.

## Transports and ecosystem features

The following are outside the current product scope:

- **MQTT 3.1** (protocol level 3; only 3.1.1 and 5.0 are supported)
- **MQTT over WebSockets**
- **Built-in `$SYS/` topics**
- **Bridge/federation** to other brokers

Plain TCP and MQTTS on the device are the supported transports.

## Possible future extensions

Depending on deployment needs, future work may include:

1. **Topic ACL** — publish/subscribe rules keyed by username, client ID, or role
2. **Persistent retained or session state** — selective flash backing for critical topics or sessions
3. **Shared subscriptions or topic aliases** — if client libraries or workloads require them
4. **Enhanced authentication** — for integrations that mandate SASL-style AUTH exchange

None of these are required for the broker's current conformance profile or typical IIoT workloads. They are noted here for roadmap and integration planning.

## Related documents

- [README](../README.md) — build, deploy, authentication, and platform limits
- [Platform comparison](reports/platform_comparison_summary.md) — on-device throughput and conformance by hardware target
- Per-platform conformance reports in [docs/reports/](reports/)
