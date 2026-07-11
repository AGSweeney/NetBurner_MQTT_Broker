# MQTT Broker Conformance Report

## Hardware

| Spec | Value |
| --- | --- |
| CPU | Microchip SAM E70 (Arm Cortex-M7) |
| Clock | 300 MHz |
| RAM | 8 MB SDRAM + 384 KB on-chip SRAM |
| Flash | 2 MB embedded NOR |
| Network | 10/100 Ethernet |
| Broker limits | 16 TCP clients · 72 KB payload pool · 32 KB retained |

| Field | Value |
| --- | --- |
| **Device** | NetBurner MODM7AE70 at `172.16.82.52` (TCP 1883) |
| **Run** | 2026-07-11 16:36:25 |
| **Result** | **28/28 tests passed** |
| **Soak** | 147,246 messages / 120 s — 100% delivered |

> Both protocol versions pass every conformance check. Sustained soaks show flat delivery rates with zero parser errors, slow-consumer disconnects, pool exhaustion, or quota drops.

## Sustained soak

![Soak delivery rate](modm7ae70_conformance_report_img/soak_rate.png)

| Metric | MQTT 3.1.1 | MQTT 5 |
|---|---|---|
| Duration | 60 s | 60 s |
| Published | 74,684 | 72,562 |
| Delivered | 74,684 (100.0%) | 72,562 (100.0%) |
| Average rate | 1,244 msg/s | 1,209 msg/s |
| Parser errors | 0 | 0 |
| Slow-consumer disconnects | 0 | 0 |
| Pool exhaustion | 0 | 0 |
| Keep-alive disconnects | 0 | 0 |
| Quota / size drops | 0 / 0 | 0 / 0 |

## Conformance results

_Durations include intentional waits (keep-alive expiry, retained-delete settling), not broker processing time._

| Test | MQTT 3.1.1 status | MQTT 3.1.1 duration | MQTT 5 status | MQTT 5 duration |
| --- | :---: | ---: | :---: | ---: |
| CONNECT / CONNACK wire format | PASS | 2.0 ms | PASS | 2.7 ms |
| PINGREQ / PINGRESP | PASS | 3.1 ms | PASS | 3.2 ms |
| QoS 0 publish and subscribe | PASS | 5.8 ms | PASS | 5.9 ms |
| QoS 1 publish with PUBACK format check | PASS | 6.6 ms | PASS | 6.8 ms |
| QoS 2 four-packet handshake, both directions | PASS | 1.0 s | PASS | 1.0 s |
| Retained message: set, deliver, delete | PASS | 1.6 s | PASS | 1.6 s |
| Will published on abrupt disconnect | PASS | 7.1 ms | PASS | 7.2 ms |
| Wildcard filters (`+` and `#`) | PASS | 7.8 ms | PASS | 7.6 ms |
| UNSUBSCRIBE stops delivery | PASS | 1.0 s | PASS | 1.0 s |
| Session persistence across reconnect | PASS | 1.0 s | PASS | 1.0 s |
| Keep-alive enforcement (1.5× timeout) | PASS | 6.5 s | PASS | 6.5 s |
| 4 KB payload integrity | PASS | 35.9 ms | PASS | 36.1 ms |

### Cross-version interoperability

| Scenario | Status | Duration |
| --- | :---: | ---: |
| MQTT 5 publisher → 3.1.1 subscriber (properties stripped) | PASS | 6.3 ms |
| 3.1.1 publisher → MQTT 5 subscriber | PASS | 6.8 ms |
| Retained by MQTT 5, consumed by 3.1.1 | PASS | 310.5 ms |
| QoS 1 delivery across versions | PASS | 7.1 ms |

## Performance benchmarks

![Sustained throughput](modm7ae70_conformance_report_img/throughput.png)

![Latency](modm7ae70_conformance_report_img/latency.png)

| Metric | MQTT 3.1.1 | MQTT 5 |
|---|---|---|
| Sustained publish rate | 1,244 msg/s | 1,209 msg/s |
| Sustained delivery rate | 1,244 msg/s | 1,209 msg/s |
| Delivery completeness | 74,684/74,684 | 72,562/72,562 |
| Latency p50 / p95 / p99 | 0.8 / 1.0 / 1.0 ms | 0.8 / 0.9 / 0.9 ms |
| Latency worst sample | 1.0 ms | 0.9 ms |

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
python mqtt_conformance_suite.py --host 172.16.82.52 --soak-seconds 60
python generate_conformance_report.py --results mqtt_conformance_results.json \
    --out-dir ../../../docs/reports
```
