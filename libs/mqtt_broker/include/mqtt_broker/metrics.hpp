#ifndef MQTT_BROKER_METRICS_HPP
#define MQTT_BROKER_METRICS_HPP

#include <cstdint>

namespace mqtt_broker {

// Runtime counters exposed via Broker::metrics() and the platform status API.
// All fields are monotonic uint32_t tallies unless noted.
struct BrokerMetrics {
    uint32_t clients_connected;          // Live connected sessions right now
    uint32_t clients_connected_peak;     // High-water mark of clients_connected
    uint32_t connect_accept;             // CONNACK Success after CONNECT
    uint32_t connect_reject;             // CONNACK reject (any non-Success reason)
    uint32_t publish_received;           // Inbound PUBLISH accepted from clients
    uint32_t publish_sent;               // Outbound PUBLISH enqueued to a client tx queue
    uint32_t dropped_too_large;          // Delivery skipped: exceeds client Maximum Packet Size
    uint32_t dropped_quota;              // Delivery skipped: offline queue, QoS inflight, or packet-id quota
    uint32_t quota_drop_count;           // Offline-queue drop-new only (enqueue_offline failure)
    uint32_t will_published;             // Will messages published on unclean disconnect
    uint32_t session_takeovers;          // Clean-session=false reconnect displaced prior session
    uint32_t pool_exhaustion;            // Retained-message store rejected (pool full)
    uint32_t keepalive_disconnects;      // Disconnect: keep-alive timer expired
    uint32_t slow_consumer_disconnects;  // Forced disconnect: client tx backlog exceeded
    uint32_t parser_errors;              // Wire parse failures or unsupported packet types
    uint32_t tx_logical_high_water;      // Peak sum of bytes queued across all client tx queues
    uint32_t messages_expired;           // Dropped or skipped: Message Expiry Interval elapsed
};

}  // namespace mqtt_broker

#endif
