# NetBurner MQTT Broker

An embedded **MQTT broker** (MQTT 3.1.1 and MQTT 5.0) for [NetBurner](https://www.netburner.com) modules. The firmware runs on-device with plain TCP and MQTTS listeners, a web admin UI, optional client authentication, and a portable C++ broker core that can be unit-tested on the host.

## Features

- **MQTT 3.1.1 and MQTT 5.0** — Dual-protocol broker on the same TCP listener; clients negotiate version at CONNECT. MQTT 5 adds CONNECT/CONNACK properties, session expiry, will messages (including DISCONNECT with Will Message), and reason codes. Both versions support QoS 0/1/2 with full handshake, retained messages, and wildcard subscriptions
- **Cross-version routing** — MQTT 5 publishers can deliver to MQTT 3.1.1 subscribers (properties stripped); retained and QoS 1 work across protocol versions
- **Dual listeners** — Plain TCP (default port 1883) and MQTTS (default port 8883, uses the device TLS certificate)
- **Sparkplug B ready** — Payload-agnostic routing; host tests and live scripts exercise Sparkplug topic shapes and binary payloads
- **Web admin UI** — Dashboard, broker settings, security (MQTT users), and network configuration at `http://<device-ip>/`
- **Client authentication** — Optional username/password with salted SHA-256 credentials stored on EFFS flash (up to 16 users)
- **Platform-tuned limits** — Compile-time capacity profiles sized per NetBurner target (clients, payload pool, retained store, TLS slots)
- **Let's Encrypt (ACME)** — Optional publicly trusted TLS on **SOMRT1061** via the Security admin UI (NNDK 3.5+)

## Architecture

The repository splits a portable broker library from the NetBurner integration layer:

```
NetBurner_MQTT_Broker/
├── libs/mqtt_broker/              # Portable MQTT broker (C++17; 3.1.1 + 5.0)
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
| **SOMRT1061** | [SOMRT1061 Development Kit](https://www.netburner.com/products/development-kit/som-dev-kits/arm-embedded-iot-development-kit-somrt1061/) | `broker_limits_somrt1061.hpp` | **Recommended**; dual Ethernet; **Let's Encrypt / ACME supported** |
| **MODM7AE70** | [MODM7AE70 Development Kit](https://www.netburner.com/products/development-kit/som-dev-kits/modm7ae70-development-kit/) | `broker_limits_modm7ae70.hpp` | Tighter RAM budget (fewer clients, smaller payload pool) |
| **MODRT1171** | [MODRT1171 Development Kit](https://www.netburner.com/products/development-kit/som-dev-kits/i-mx-rt1171-embedded-iot-development-kit-modrt1171/) | `broker_limits_somrt1061.hpp` | **Recommended**; newest i.MX RT platform; self-signed and user PEM only (no ACME in v1) |
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
make PLATFORM=SOMRT1061 # recommended; required for Let's Encrypt (ACME)
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

### mqtt_device_suite

From `libs/mqtt_broker`:

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target mqtt_device_suite
.\build\tools\device_suite\Release\mqtt_device_suite.exe --host 172.16.82.8 --soak-seconds 3600
```

Conformance (3.1.1 + 5), benchmarks, optional soak. Default host `172.16.82.8:1883`. JSON output via `--out`. See `--help`.

### Node scripts

From `platforms/netburner/scripts/` (`npm install` first). Device IP is the first argument; default `172.16.82.8`.

```bash
node mqtt5_verify.js <device-ip>          # plain TCP :1883
node mqtt5_tls_verify.js <device-ip>      # MQTTS :8883
node sparkplug_live_verify.js <device-ip>
node mqtt5_auth_verify.js <device-ip>     # set up users in admin UI first
```

Other scripts in that directory: `mqtt-smoke.ps1`, `mqtt-bench.py`, etc.

### Reports

[docs/reports/platform_comparison_summary.md](docs/reports/platform_comparison_summary.md) — SOMRT1061, MODM7AE70, NANO54415. Per-platform reports in the same folder.

```powershell
cd platforms/netburner/scripts
python generate_conformance_report.py --results ../../../libs/mqtt_broker/somrt1061_soak_results.json --out-dir ../../../docs/reports --name somrt1061_conformance_report --platform SOMRT1061
python generate_platform_summary.py --results-dir ../../../libs/mqtt_broker --out-dir ../../../docs/reports
```

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

## Broker Capabilities

### MQTT 5 (CONNACK)

Advertised to connecting MQTT 5 clients:

- **Maximum QoS:** 2
- **Retain available:** yes
- **Wildcard subscriptions:** yes
- **Shared subscriptions:** no
- **Topic aliases:** not supported
- **Receive Maximum:** 8 (matches per-client QoS 2 inflight limit)
- **Maximum Packet Size:** 16,384 bytes

### MQTT 3.1.1

Same TCP/MQTTS ports as MQTT 5. Protocol level 4 (`MQIsdp` / `MQTT`). QoS 0–2, wills, retained, wildcards. No MQTT 5 properties or reason codes.

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

## TLS certificate sources

The same server certificate secures HTTPS (port 443) and MQTTS when enabled.

| Source | Platforms | Use case |
|--------|-----------|----------|
| Auto-generated (self-signed) | All | LAN / lab; no public DNS |
| User PEM upload | All | Corporate CA or manually provisioned cert |
| **Let's Encrypt (ACME)** | **SOMRT1061 only** | Internet-facing broker with public DNS |

## Let's Encrypt setup (SOMRT1061)

Automatic TLS certificate enrollment uses the NetBurner NNDK ACME servlet ([NetBurner ACME tutorial](https://www.netburner.com/learn/new-feature-easy-ssl-certificates-with-acme-and-lets-encrypt/)).

### Requirements

- NNDK **3.5+** with ACME servlet support
- Build with `make PLATFORM=SOMRT1061` (ACME is linked only on this platform)
- Valid **NTP** time (configured automatically at boot)
- A **public DNS name** pointing at the device
- Port **80** reachable from the Internet (HTTP-01 challenge)
- Port **443** for HTTPS and MQTTS after enrollment

### Enable from the admin UI

1. Open **Security** → **Certificates**
2. Set **Certificate source** to **Let's Encrypt (ACME)**
3. Enter **Common name**, **Alt names** (comma-separated SANs), and **Contact email**
4. Optionally enable **Staging** for Let's Encrypt staging CA (testing, higher rate limits)
5. Click **Apply Certificate Source** or **Save Let's Encrypt Settings** — the device reboots

### Provisioning flow

NTP sync → ACME HTTP-01 enrollment (port 80 must stay up) → HTTPS and MQTTS become active with a browser-trusted certificate. Poll enrollment status on the Security tab or via `GET /api/ssl.json` (`acme.state`, `httpsService.status`).

### REST API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/ssl.json` | GET | Includes `acme.supported`, `acme.enabled`, `acme.state`, and certificate source |
| `/api/ssl/acme` | POST | Save ACME settings and switch certificate source (reboot) |
| `/api/ssl/acme/retry` | POST | Restart ACME enrollment (reboot) |

On non-SOMRT1061 builds, `acme.supported` is `false` and ACME endpoints return HTTP 400.

### MQTTS with Let's Encrypt

MQTTS uses the same certificate as HTTPS. Clients can use standard CA trust stores (no `rejectUnauthorized: false`) when enrolled with the production Let's Encrypt CA:

```bash
node platforms/netburner/scripts/mqtt5_tls_verify.js broker.example.com --verify-ca
```

## TLS / MQTTS

MQTTS reuses the device HTTPS certificate managed by the NetBurner SSL service. Certificate sources include user-installed PEM, HAL auto-generated, self-signed (after NTP sync), and **Let's Encrypt on SOMRT1061** — see [TLS certificate sources](#tls-certificate-sources) and [Let's Encrypt setup (SOMRT1061)](#lets-encrypt-setup-somrt1061).

Enable the TLS listener in broker settings once `SslCertReady()` is true. TLS client capacity is limited (`MaxTlsClients = 2` on reference platforms) due to handshake RAM cost on embedded targets.

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
