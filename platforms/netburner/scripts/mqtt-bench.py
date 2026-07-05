# Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
# SPDX-License-Identifier: MIT

#!/usr/bin/env python3
"""Measure MQTT publish rate to nb-mqtt-broker (QoS 0, MQTT 5)."""

import argparse
import socket
import struct
import time


def encode_varint(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            b |= 0x80
        out.append(b)
        if not n:
            break
    return bytes(out)


def build_connect(client_id: str, keep_alive: int = 60) -> bytes:
    body = bytearray()
    body += b"\x00\x04MQTT\x05\x02"
    body += struct.pack(">H", keep_alive)
    body += b"\x00"  # properties length
    cid = client_id.encode()
    body += struct.pack(">H", len(cid)) + cid
    return bytes([0x10]) + encode_varint(len(body)) + body


def build_publish(topic: str, payload: bytes) -> bytes:
    body = bytearray()
    t = topic.encode()
    body += struct.pack(">H", len(t)) + t
    body += b"\x00"  # properties length
    body += payload
    return bytes([0x30]) + encode_varint(len(body)) + body


def build_disconnect() -> bytes:
    return b"\xE0\x00"


def run_once(host: str, port: int, count: int, delay_s: float, payload_size: int) -> dict:
    client_id = f"bench-{int(time.time() * 1000) % 1000000}"
    topic = "test/bench"

    sock = socket.create_connection((host, port), timeout=5)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    t0 = time.perf_counter()
    sock.sendall(build_connect(client_id))
    connack = sock.recv(64)
    if len(connack) < 2 or connack[0] != 0x20:
        sock.close()
        raise RuntimeError(f"CONNACK failed: {connack.hex()}")

    sent = 0
    for i in range(count):
        p = b'{"seq":%d,"ts":%.6f}' % (i, time.time())
        if len(p) < payload_size:
            p += b" " * (payload_size - len(p))
        elif len(p) > payload_size:
            p = p[:payload_size]
        sock.sendall(build_publish(topic, p))
        sent += 1
        if delay_s > 0 and i + 1 < count:
            time.sleep(delay_s)

    sock.sendall(build_disconnect())
    elapsed = time.perf_counter() - t0
    sock.close()

    return {
        "count": sent,
        "elapsed_s": elapsed,
        "rate_hz": sent / elapsed if elapsed > 0 else 0.0,
        "delay_ms": delay_s * 1000.0,
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="172.16.82.55")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--payload", type=int, default=48, help="payload bytes")
    ap.add_argument("--delays", default="0,1,5", help="comma-separated delay ms")
    args = ap.parse_args()

    delays_ms = [float(x.strip()) for x in args.delays.split(",") if x.strip()]
    print(f"Broker {args.host}:{args.port}  count={args.count}  payload={args.payload}B")
    print(f"{'delay_ms':>8}  {'elapsed_s':>10}  {'rate_hz':>10}  {'ok':>4}")
    print("-" * 40)

    for dms in delays_ms:
        try:
            r = run_once(args.host, args.port, args.count, dms / 1000.0, args.payload)
            print(
                f"{r['delay_ms']:8.1f}  {r['elapsed_s']:10.3f}  {r['rate_hz']:10.1f}  ok"
            )
        except OSError as e:
            print(f"{dms:8.1f}  {'FAIL':>10}  {'':>10}  {e}")


if __name__ == "__main__":
    main()
