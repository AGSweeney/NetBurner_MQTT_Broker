# Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
# SPDX-License-Identifier: MIT

#!/usr/bin/env python3
"""End-to-end MQTT publish + subscribe throughput benchmark."""

import argparse
import socket
import struct
import threading
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
    body = bytearray(b"\x00\x04MQTT\x05\x02")
    body += struct.pack(">H", keep_alive)
    body += b"\x00"
    cid = client_id.encode()
    body += struct.pack(">H", len(cid)) + cid
    return bytes([0x10]) + encode_varint(len(body)) + body


def build_subscribe(packet_id: int, topic: str) -> bytes:
    filt = topic.encode()
    body = struct.pack(">H", packet_id) + b"\x00"
    body += struct.pack(">H", len(filt)) + filt + b"\x00"
    return bytes([0x82]) + encode_varint(len(body)) + body


def build_publish(topic: str, payload: bytes) -> bytes:
    t = topic.encode()
    body = struct.pack(">H", len(t)) + t + b"\x00" + payload
    return bytes([0x30]) + encode_varint(len(body)) + body


def build_disconnect() -> bytes:
    return b"\xE0\x00"


def read_packet(sock: socket.socket, timeout: float = 10.0) -> bytes:
    sock.settimeout(timeout)
    buf = bytearray()
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            chunk = sock.recv(4096)
            if not chunk:
                return bytes(buf)
            buf.extend(chunk)
            if len(buf) >= 2:
                mult = 1
                rem = 0
                idx = 1
                while idx < len(buf) and idx < 5:
                    b = buf[idx]
                    rem += (b & 0x7F) * mult
                    mult *= 128
                    idx += 1
                    if not (b & 0x80):
                        break
                need = idx + rem
                if len(buf) >= need:
                    return bytes(buf[:need])
        except socket.timeout:
            break
    return bytes(buf)


def bench_host(host: str, count: int, delay_ms: float = 0.0) -> dict:
    topic = "test/bench"

    sub = socket.create_connection((host, 1883), timeout=5)
    sub.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sub.sendall(build_connect("bench-sub"))
    read_packet(sub, 3)
    sub.sendall(build_subscribe(1, topic))
    read_packet(sub, 3)

    pub = socket.create_connection((host, 1883), timeout=5)
    pub.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    pub.sendall(build_connect("bench-pub"))
    read_packet(pub, 3)

    received = {"n": 0, "first": None, "last": None}
    stop = {"v": False}

    def reader() -> None:
        while not stop["v"]:
            pkt = read_packet(sub, 1.0)
            if not pkt:
                continue
            if (pkt[0] & 0xF0) == 0x30:
                now = time.perf_counter()
                if received["first"] is None:
                    received["first"] = now
                received["last"] = now
                received["n"] += 1

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()
    time.sleep(0.05)

    t0 = time.perf_counter()
    for i in range(count):
        payload = ('{"seq":%d}' % i).encode()
        pub.sendall(build_publish(topic, payload))
        if delay_ms > 0 and i + 1 < count:
            time.sleep(delay_ms / 1000.0)
    pub.sendall(build_disconnect())
    pub.close()

    deadline = time.time() + 8.0
    while received["n"] < count and time.time() < deadline:
        time.sleep(0.005)

    stop["v"] = True
    wall = time.perf_counter() - t0
    delivery_rate = 0.0
    if received["first"] is not None and received["last"] is not None:
        span = max(received["last"] - received["first"], 0.001)
        delivery_rate = received["n"] / span

    sub.sendall(build_disconnect())
    sub.close()

    return {
        "host": host,
        "count": count,
        "delivered": received["n"],
        "publish_rate": count / wall if wall > 0 else 0.0,
        "delivery_rate": delivery_rate,
        "wall_s": wall,
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--hosts", default="172.16.82.52,172.16.82.55")
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--delay-ms", type=float, default=5.0)
    args = ap.parse_args()

    hosts = [h.strip() for h in args.hosts.split(",") if h.strip()]
    print(
        f"End-to-end benchmark: {args.count} publishes, QoS 0, delay={args.delay_ms}ms"
    )
    print(f"{'host':<16}  {'delivered':>10}  {'pub msg/s':>10}  {'deliver msg/s':>13}  {'wall_s':>8}")
    print("-" * 64)

    for host in hosts:
        try:
            r = bench_host(host, args.count, args.delay_ms)
            print(
                f"{r['host']:<16}  {r['delivered']:>10}/{args.count}  "
                f"{r['publish_rate']:>10.1f}  {r['delivery_rate']:>13.1f}  {r['wall_s']:>8.3f}"
            )
        except OSError as exc:
            print(f"{host:<16}  FAIL: {exc}")


if __name__ == "__main__":
    main()
