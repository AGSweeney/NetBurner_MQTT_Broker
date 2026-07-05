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

| Platform | Limits profile | Notes |
|----------|----------------|-------|
| **NANO54415** | `broker_limits_nano54415.hpp` | Default makefile target; primary reference hardware |
| **SOMRT1061** | `broker_limits_somrt1061.hpp` | RT platform; EFFS mounted by platform library |
| **MODM7AE70** | `broker_limits_modm7ae70.hpp` | Tighter RAM budget (fewer clients, smaller payload pool) |
| **MODRT1171** | Uses SOMRT1061-style EFFS handling | Build with `PLATFORM=MODRT1171` |
| **MOD5441X**, **SB800EX** | NANO54415 limits (via makefile flash layout) | ColdFire / alternate flash geometry |

Host unit tests use `MQTT_BROKER_HOST_LE` with limits aligned to NANO54415.

## Requirements

### Firmware build

- [NetBurner Network Development Kit (NNDK)](https://www.netburner.com/NNDK/) installed with `NNDK_ROOT` set
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
make PLATFORM=SOMRT1061
make PLATFORM=MODM7AE70
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

## License

No license file is included in this repository. Contact the repository owner for licensing terms.
