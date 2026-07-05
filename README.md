# NetBurner MQTT Broker

An embedded **MQTT 5.0 broker** for [NetBurner](https://www.netburner.com) modules. The firmware runs on-device with plain TCP and MQTTS listeners, a web admin UI, optional client authentication, and a portable C++ broker core that can be unit-tested on the host.

## Features

- **MQTT 5.0** — CONNECT/CONNACK properties, session expiry, will messages (including DISCONNECT with Will Message), QoS 0/1/2 with full handshake, retained messages, wildcard subscriptions
- **Dual listeners** — Plain TCP (default port 1883) and MQTTS (default port 8883, uses the device TLS certificate)
- **Sparkplug B ready** — Payload-agnostic routing; host tests and live scripts exercise Sparkplug topic shapes and binary payloads
- **Web admin UI** — Dashboard, broker settings, security (MQTT users), and network configuration at `http://<device-ip>/`
- **Client authentication** — Optional username/password with salted SHA-256 credentials stored on EFFS flash (up to 16 users)
- **Platform-tuned limits** — Compile-time capacity profiles sized per NetBurner target (clients, payload pool, retained store, TLS slots)

## Architecture

The repository splits a portable broker library from the NetBurner integration layer:

```
NetBurner_MQTT_Broker/
├── libs/mqtt_broker/              # Portable MQTT 5 broker (C++17)
│   ├── include/mqtt_broker/       # Public headers (Broker, parser, session, limits, …)
│   ├── src/                       # Broker implementation
│   ├── tests/                     # Host unit & acceptance tests (CMake/CTest)
│   └── CMakeLists.txt
└── platforms/netburner/
    ├── nb-mqtt-broker/            # NetBurner firmware application
    │   ├── src/                   # TCP/TLS server, config, auth, web API, SSL
    │   ├── html/                  # Admin UI (compiled into firmware)
    │   └── makefile
    └── scripts/                   # Node.js / Python device verification tools
```

The portable `mqtt_broker::Broker` class owns sessions, subscriptions, retained messages, and per-connection parsers. It is **single-threaded** — the platform attaches transports (TCP/TLS sockets), calls `on_readable()` when data arrives, `tick()` for timers, and `drain_tx()` to flush outbound packets.

On NetBurner, `BrokerServerTask` drives the broker from one RTOS task using `select()` on plain and TLS listen/client sockets.

## Supported Platforms

| Platform | Development kit | Limits profile | Notes |
|----------|-----------------|----------------|-------|
| **NANO54415** | [NANO54415 Development Kit](https://www.netburner.com/products/development-kit/som-dev-kits/nano54415-development-kit/) | `broker_limits_nano54415.hpp` | Default makefile target; primary reference hardware |
| **SOMRT1061** | [SOMRT1061 Development Kit](https://www.netburner.com/products/development-kit/som-dev-kits/arm-embedded-iot-development-kit-somrt1061/) | `broker_limits_somrt1061.hpp` | **Recommended** for new designs; dual Ethernet, validated on hardware |
| **MODM7AE70** | [MODM7AE70 Development Kit](https://www.netburner.com/products/development-kit/som-dev-kits/modm7ae70-development-kit/) | `broker_limits_modm7ae70.hpp` | Tighter RAM budget (fewer clients, smaller payload pool) |
| **MODRT1171** | [MODRT1171 Development Kit](https://www.netburner.com/products/development-kit/som-dev-kits/i-mx-rt1171-embedded-iot-development-kit-modrt1171/) | `broker_limits_somrt1061.hpp` | **Recommended** for new designs; newest i.MX RT platform |
| **MOD5441X** | [MOD54415 LC Development Kit](https://www.netburner.com/products/development-kit/som-dev-kits/mod54415-lc-development-kit/) | `broker_limits_nano54415.hpp` | ColdFire platform; legacy deployments |

Host unit tests use `MQTT_BROKER_HOST_LE` with limits aligned to NANO54415.

### Recommended platforms

For **new projects**, start with one of the modern NXP i.MX RT modules:

- **[SOMRT1061](https://www.netburner.com/products/development-kit/som-dev-kits/arm-embedded-iot-development-kit-somrt1061/)** — Best starting point. Dual 10/100 Ethernet, the highest broker capacity profile in this tree, and the platform used for on-device validation and sizing.
- **[MODRT1171](https://www.netburner.com/products/development-kit/som-dev-kits/i-mx-rt1171-embedded-iot-development-kit-modrt1171/)** — Newest NetBurner RT kit (i.MX RT1171). Uses the same RT integration path and broker limits as SOMRT1061; prefer this when you want the latest hardware generation and headroom.

**NANO54415** remains the default `make` target and the reference profile for host unit tests. It is well supported but ColdFire-based; choose it when you already deploy NANO54415 hardware rather than for greenfield designs.

**MODM7AE70** and **MOD5441X** remain supported for existing installs or specific form-factor needs.

## Requirements

### Firmware build

- [NetBurner Network Development Kit (NNDK)](https://www.netburner.com/get-tools/) installed with `NNDK_ROOT` set
- A supported NetBurner module (see [Supported Platforms](#supported-platforms) for development kit links)
- NetBurner IDE or command-line `make` from the NNDK toolchain

### Host unit tests

- CMake 3.16+
- C++17 compiler (MSVC, GCC, or Clang)

### Device verification scripts

- Node.js 18+ (for `fetch` in TLS verify script)
- `npm install` in `platforms/netburner/scripts/` (installs `mqtt` v5 client)

## Building Firmware

From the NetBurner application directory:

```bash
cd platforms/netburner/nb-mqtt-broker
make clean
make                    # default PLATFORM=NANO54415
make PLATFORM=SOMRT1061 # recommended
make PLATFORM=MODRT1171 # recommended (newest RT platform)
make PLATFORM=MODM7AE70
make PLATFORM=MOD5441X
```

The makefile compiles the portable broker sources from `libs/mqtt_broker/` and generates `src/htmldata.cpp` from the files in `html/` via `comphtml`.

Load the resulting image onto your module with the NetBurner IDE or your usual deployment workflow.

## Running Host Unit Tests

```bash
cd libs/mqtt_broker
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

On Windows, the helper script runs the same suite with assert-dialog suppression:

```powershell
.\libs\mqtt_broker\scripts\run-ctest.ps1
```

### Test coverage

| Test | Focus |
|------|-------|
| `test_wire`, `test_varint`, `test_parser` | Wire encoding and packet parsing |
| `test_message_pool`, `test_tx_queue`, `test_topic_*` | Memory pools, routing trie, topic interning |
| `test_broker`, `test_session` | End-to-end broker behavior |
| `test_sparkplug_server` | Sparkplug B lifecycle (wills, QoS 1 retained, binary payloads) |
| `test_qos_downgrade`, `test_properties` | QoS and MQTT 5 properties |
| `fuzz_parser` | Parser robustness |

## Device Verification

After flashing and connecting the module to your network, run scripts from `platforms/netburner/scripts/`. Pass the device IP as the first argument (scripts default to `172.16.82.8`).

```bash
cd platforms/netburner/scripts
npm install

# MQTT 5 conformance (plain TCP :1883)
node mqtt5_verify.js <device-ip>

# MQTTS on :8883 (enable TLS in admin UI first)
node mqtt5_tls_verify.js <device-ip>

# Sparkplug B lifecycle over MQTT 5
node sparkplug_live_verify.js <device-ip>

# Authentication (configure users in admin UI first)
node mqtt5_auth_verify.js <device-ip>
```

Additional PowerShell and Python utilities (`mqtt-smoke.ps1`, `mqtt-bench.py`, etc.) are available for load and qualification testing.

## Configuration

On first boot, open the admin UI at **`http://<device-ip>/`**. When a TLS certificate is available, HTTPS is also served using the same certificate used for MQTTS.

### Broker settings (persisted via NetBurner `config_server`)

| Setting | Default | Description |
|---------|---------|-------------|
| Plain TCP enabled | `true` | MQTT listener on plain TCP |
| Plain TCP port | `1883` | Plain MQTT port |
| TLS enabled | `false` | MQTTS listener |
| TLS port | `8883` | MQTTS port |
| Auth required | `false` | Reject CONNECT without credentials when enabled |
| Allow anonymous | `false` | Permit unauthenticated clients when auth is required |
| Max keep-alive | `30` s | Server-side keep-alive cap |
| Connect handshake timeout | `15` s | CONNECT completion deadline |

Changes take effect when listeners are restarted (save via the Broker tab in the admin UI).

### Status API

Live metrics and compile-time limits are exposed as JSON:

```
GET /api/broker/status
```

The dashboard polls this endpoint every two seconds.

## Broker Capabilities (CONNACK)

Advertised to connecting MQTT 5 clients:

- **Maximum QoS:** 2
- **Retain available:** yes
- **Wildcard subscriptions:** yes
- **Shared subscriptions:** no
- **Topic aliases:** not supported
- **Receive Maximum:** 8 (matches per-client QoS 2 inflight limit)
- **Maximum Packet Size:** 16,384 bytes

## Platform Limits (NANO54415 / SOMRT1061 reference)

| Limit | Value |
|-------|-------|
| Max TCP clients | 32 |
| Max TLS clients | 2 |
| Max subscriptions per client | 16 |
| Max packet size | 16,384 B |
| Max payload size | 8,192 B |
| Payload pool | 128 KB (256 × 512 B blocks) |
| Retained store | 64 KB total |
| Offline queue per client | 16 messages |

MODM7AE70 uses reduced values (16 TCP clients, 72 KB payload pool, 32 KB retained store). See the `broker_limits_*.hpp` headers for full per-platform tables.

## MQTT Authentication

When authentication is enabled, credentials are managed from the **Security** tab in the admin UI. Passwords are stored as `SHA-256(salt || password)` with a random per-user salt on EFFS flash. Plaintext passwords are never written to storage.

The broker invokes a platform-supplied auth handler at CONNECT time; failed verification returns CONNACK reason `BadUserNameOrPassword` or `NotAuthorized`.

## TLS / MQTTS

MQTTS reuses the device HTTPS certificate managed by the NetBurner SSL service (user-installed, HAL auto-generated, or self-signed after NTP sync). Enable the TLS listener in broker settings once `SslCertReady()` is true. TLS client capacity is limited (`MaxTlsClients = 2` on reference platforms) due to handshake RAM cost on embedded targets.

## Disclaimer

This project is an independent open-source effort by Adam G. Sweeney. It is **not**
affiliated with, endorsed by, or sponsored by NetBurner, Inc. "NetBurner" and
related product names are trademarks of NetBurner, Inc.

## License

Copyright (c) 2026 Adam G. Sweeney (agsweeney@gmail.com)

Original code in this repository (primarily the portable MQTT broker under
`libs/mqtt_broker/` and the NetBurner broker integration) is licensed under the
[MIT License](LICENSE).

**Third-party code is not covered by MIT.** Portions of the NetBurner firmware
application were adapted from NetBurner NNDK examples (EFFS-STD file system,
SSL/TLS bootstrap, and web admin UI). Those files retain NetBurner or HCC
Embedded copyright notices and their original license terms. See
[NOTICE](NOTICE) for a file-level breakdown.

Building and running the firmware also requires the NetBurner NNDK, which is
licensed separately by NetBurner, Inc.
