// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef MQTT_BROKER_TRANSPORT_HPP
#define MQTT_BROKER_TRANSPORT_HPP

// Platform integration contract for a single TCP (or TLS) client connection.
// The broker never opens sockets itself; the platform registers read/write/close
// callbacks and drives I/O via Broker::on_readable() and Broker::drain_tx().

#include <cstddef>
#include <cstdint>

namespace mqtt_broker {

struct BrokerTransportOps {
    // Read up to cap bytes into buf. Return byte count (>0), 0 if no data yet
    // (would block), or -1 on fatal error (broker closes the connection).
    int (*read)(void *ctx, uint8_t *buf, size_t cap);
    // Write len bytes. Return bytes written (>0), 0 if the socket buffer is full
    // (broker retries on the next drain_tx), or -1 on fatal error.
    int (*write)(void *ctx, const uint8_t *data, size_t len);
    void (*close_conn)(void *ctx);
};

struct BrokerTransport {
    void *ctx;                  // Opaque socket/state passed to every op
    BrokerTransportOps ops;
    bool active;                // False after detach or broker-initiated close
};

}  // namespace mqtt_broker

#endif