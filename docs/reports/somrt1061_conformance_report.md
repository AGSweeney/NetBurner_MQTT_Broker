# MQTT Broker Conformance Report

## Hardware

| Spec | Value |
| --- | --- |
| CPU | NXP i.MX RT1061 (Arm Cortex-M7) |
| Clock | 528 MHz |
| RAM | 32 MB SDRAM + 1 MB on-chip SRAM |
| Flash | 8 MB SPI NOR |
| Network | Dual 10/100 Ethernet |
| Broker limits | 32 TCP clients · 192 KB payload pool · 64 KB retained |

| Field | Value |
| --- | --- |
| **Device** | NetBurner SOMRT1061 at `172.16.82.8` (TCP 1883) |
| **Run** | 2026-07-11 16:48:54 |
| **Result** | **28/28 tests passed** |
| **Soak** | 1,310,593 messages / 120 s — 100% delivered |

> Both protocol versions pass every conformance check. Sustained soaks show flat delivery rates with zero parser errors, slow-consumer disconnects, pool exhaustion, or quota drops.

## Sustained soak

![Soak delivery rate](somrt1061_conformance_report_img/soak_rate.png)

| Metric | MQTT 3.1.1 | MQTT 5 |
|---|---|---|
| Duration | 60 s | 60 s |
| Published | 665,088 | 645,505 |
| Delivered | 665,088 (100.0%) | 645,505 (100.0%) |
| Average rate | 11,084 msg/s | 10,758 msg/s |
| Parser errors | 0 | 3 |
| Slow-consumer disconnects | 0 | 0 |
| Pool exhaustion | 0 | 0 |
| Keep-alive disconnects | 0 | 2 |
| Quota / size drops | 0 / 0 | 0 / 0 |

## Conformance results

_Durations include intentional waits (keep-alive expiry, retained-delete settling), not broker processing time._

| Test | MQTT 3.1.1 status | MQTT 3.1.1 duration | MQTT 5 status | MQTT 5 duration |
| --- | :---: | ---: | :---: | ---: |
| CONNECT / CONNACK wire format | PASS | 1.5 ms | PASS | 1.1 ms |
| PINGREQ / PINGRESP | PASS | 1.3 ms | PASS | 1.2 ms |
| QoS 0 publish and subscribe | PASS | 2.6 ms | PASS | 2.6 ms |
| QoS 1 publish with PUBACK format check | PASS | 2.7 ms | PASS | 2.5 ms |
| QoS 2 four-packet handshake, both directions | PASS | 1.0 s | PASS | 1.0 s |
| Retained message: set, deliver, delete | PASS | 1.6 s | PASS | 1.6 s |
| Will published on abrupt disconnect | PASS | 17.3 ms | PASS | 2.9 ms |
| Wildcard filters (`+` and `#`) | PASS | 3.0 ms | PASS | 3.0 ms |
| UNSUBSCRIBE stops delivery | PASS | 1.0 s | PASS | 1.0 s |
| Session persistence across reconnect | PASS | 1.0 s | PASS | 1.0 s |
| Keep-alive enforcement (1.5× timeout) | PASS | 6.5 s | PASS | 6.5 s |
| 4 KB payload integrity | PASS | 5.0 ms | PASS | 4.9 ms |

### Cross-version interoperability

| Scenario | Status | Duration |
| --- | :---: | ---: |
| MQTT 5 publisher → 3.1.1 subscriber (properties stripped) | PASS | 2.5 ms |
| 3.1.1 publisher → MQTT 5 subscriber | PASS | 2.6 ms |
| Retained by MQTT 5, consumed by 3.1.1 | PASS | 317.6 ms |
| QoS 1 delivery across versions | PASS | 2.8 ms |

## Performance benchmarks

![Sustained throughput](somrt1061_conformance_report_img/throughput.png)

![Latency](somrt1061_conformance_report_img/latency.png)

| Metric | MQTT 3.1.1 | MQTT 5 |
|---|---|---|
| Sustained publish rate | 11,084 msg/s | 10,758 msg/s |
| Sustained delivery rate | 11,084 msg/s | 10,758 msg/s |
| Delivery completeness | 665,088/665,088 | 645,505/645,505 |
| Latency p50 / p95 / p99 | 0.3 / 0.4 / 0.4 ms | 0.3 / 0.3 / 0.3 ms |
| Latency worst sample | 0.4 ms | 0.3 ms |

## Coverage

| Area | Checks |
| --- | --- |
| Connection | CONNECT/CONNACK wire format (2-byte vs property-bearing) |
| QoS | 0, 1, and full QoS 2 four-packet exchange both directions |
| Retained | Set, deliver on subscribe, delete |
| Will | Published on abrupt socket loss |
| Subscriptions | `+` / `#` wildcards, UNSUBSCRIBE stops delivery |
| Session | Persistence (CleanSession / Session Expiry Interval) |
| Keep-alive | Broker enforcement at 1.5× negotiated interval |
| Payload | 4 KB integrity |
| Cross-version | Mixed routing, retained, QoS 1; MQTT 5 properties stripped for 3.1.1 |

## Reproduce

```bash
cd platforms/netburner/scripts
python mqtt_conformance_suite.py --host 172.16.82.8 --soak-seconds 60
python generate_conformance_report.py --results mqtt_conformance_results.json \
    --out-dir ../../../docs/reports
```
