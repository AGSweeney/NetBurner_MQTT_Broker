# MQTT Broker Conformance Report

## Hardware

| Spec | Value |
| --- | --- |
| CPU | Freescale ColdFire MCF54415 |
| Clock | 250 MHz |
| RAM | 64 MB DDR2 SDRAM |
| Flash | 8 MB parallel NOR |
| Network | 10/100 Ethernet |
| Broker limits | 32 TCP clients · 128 KB payload pool · 64 KB retained |

| Field | Value |
| --- | --- |
| **Device** | NetBurner NANO54415 at `172.16.82.55` (TCP 1883) |
| **Run** | 2026-07-11 16:26:42 |
| **Result** | **28/28 tests passed** |
| **Soak** | 98,849 messages / 120 s — 100% delivered |

> Both protocol versions pass every conformance check. Sustained soaks show flat delivery rates with zero parser errors, slow-consumer disconnects, pool exhaustion, or quota drops.

## Sustained soak

![Soak delivery rate](nano54415_conformance_report_img/soak_rate.png)

| Metric | MQTT 3.1.1 | MQTT 5 |
|---|---|---|
| Duration | 60 s | 60 s |
| Published | 50,308 | 48,541 |
| Delivered | 50,308 (100.0%) | 48,541 (100.0%) |
| Average rate | 838 msg/s | 808 msg/s |
| Parser errors | 0 | 0 |
| Slow-consumer disconnects | 0 | 0 |
| Pool exhaustion | 0 | 0 |
| Keep-alive disconnects | 0 | 0 |
| Quota / size drops | 0 / 0 | 0 / 0 |

## Conformance results

_Durations include intentional waits (keep-alive expiry, retained-delete settling), not broker processing time._

| Test | MQTT 3.1.1 status | MQTT 3.1.1 duration | MQTT 5 status | MQTT 5 duration |
| --- | :---: | ---: | :---: | ---: |
| CONNECT / CONNACK wire format | PASS | 2.8 ms | PASS | 4.6 ms |
| PINGREQ / PINGRESP | PASS | 5.7 ms | PASS | 5.6 ms |
| QoS 0 publish and subscribe | PASS | 10.9 ms | PASS | 11.1 ms |
| QoS 1 publish with PUBACK format check | PASS | 12.3 ms | PASS | 12.6 ms |
| QoS 2 four-packet handshake, both directions | PASS | 1.0 s | PASS | 1.0 s |
| Retained message: set, deliver, delete | PASS | 1.6 s | PASS | 1.6 s |
| Will published on abrupt disconnect | PASS | 12.1 ms | PASS | 12.7 ms |
| Wildcard filters (`+` and `#`) | PASS | 14.2 ms | PASS | 14.6 ms |
| UNSUBSCRIBE stops delivery | PASS | 1.0 s | PASS | 1.0 s |
| Session persistence across reconnect | PASS | 1.0 s | PASS | 1.0 s |
| Keep-alive enforcement (1.5× timeout) | PASS | 6.5 s | PASS | 6.5 s |
| 4 KB payload integrity | PASS | 50.7 ms | PASS | 51.0 ms |

### Cross-version interoperability

| Scenario | Status | Duration |
| --- | :---: | ---: |
| MQTT 5 publisher → 3.1.1 subscriber (properties stripped) | PASS | 11.7 ms |
| 3.1.1 publisher → MQTT 5 subscriber | PASS | 12.0 ms |
| Retained by MQTT 5, consumed by 3.1.1 | PASS | 321.8 ms |
| QoS 1 delivery across versions | PASS | 13.0 ms |

## Performance benchmarks

![Sustained throughput](nano54415_conformance_report_img/throughput.png)

![Latency](nano54415_conformance_report_img/latency.png)

| Metric | MQTT 3.1.1 | MQTT 5 |
|---|---|---|
| Sustained publish rate | 838 msg/s | 808 msg/s |
| Sustained delivery rate | 838 msg/s | 809 msg/s |
| Delivery completeness | 50,308/50,308 | 48,541/48,541 |
| Latency p50 / p95 / p99 | 1.4 / 1.6 / 1.6 ms | 1.4 / 1.5 / 1.5 ms |
| Latency worst sample | 1.6 ms | 1.5 ms |

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
python mqtt_conformance_suite.py --host 172.16.82.55 --soak-seconds 60
python generate_conformance_report.py --results mqtt_conformance_results.json \
    --out-dir ../../../docs/reports
```
