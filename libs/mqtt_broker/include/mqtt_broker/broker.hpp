// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef MQTT_BROKER_BROKER_HPP
#define MQTT_BROKER_BROKER_HPP

// Central MQTT 5 broker: owns sessions, subscriptions, retained store, and per-
// connection parsers/TX queues. Platform code attaches transports (TCP/TLS),
// calls on_readable() when data arrives, tick() for timers, and drain_tx() to
// flush outbound packets. Single-threaded — no internal locking.

#include "mqtt_broker/broker_limits.hpp"
#include "mqtt_broker/inflight.hpp"
#include "mqtt_broker/message_pool.hpp"
#include "mqtt_broker/offline_queue.hpp"
#include "mqtt_broker/metrics.hpp"
#include "mqtt_broker/parser.hpp"
#include "mqtt_broker/property_pool.hpp"
#include "mqtt_broker/retained_store.hpp"
#include "mqtt_broker/session.hpp"
#include "mqtt_broker/topic_intern.hpp"
#include "mqtt_broker/topic_trie.hpp"
#include "mqtt_broker/transport.hpp"
#include "mqtt_broker/tx_queue.hpp"

#include <cstddef>
#include <cstdint>

namespace mqtt_broker {

class Broker {
public:
    Broker();
    ~Broker();

    Broker(const Broker &) = delete;
    Broker &operator=(const Broker &) = delete;

    // Register a new client connection. Returns transport_id for subsequent calls.
    uint16_t attach_transport(const BrokerTransport &transport);
    void detach_transport(uint16_t transport_id);

    // Read and parse inbound bytes; may enqueue outbound replies immediately.
    void on_readable(uint16_t transport_id);
    // Advance keep-alive, session/will expiry, and retained message expiry.
    void tick(uint32_t now_ticks);
    // Write queued outbound packets until the transport blocks or the queue empties.
    void drain_tx(uint16_t transport_id);

    const BrokerMetrics &metrics() const { return metrics_; }
    size_t connected_count() const;
    bool transport_attached(uint16_t transport_id) const;
    bool has_pending_tx() const;

#if defined(MQTT_BROKER_HOST_LE)
    uint32_t debug_client_max_packet(uint16_t transport_id) const;
    int16_t debug_find_session(const char *client_id) const;
    bool debug_session_present(const char *client_id) const;
    uint32_t debug_offline_queue_depth(uint16_t session_slot) const;
    bool debug_enqueue_offline(uint16_t session_slot, const PendingDelivery &pd);
    bool debug_fill_offline_queue(uint16_t session_slot, size_t count);

    // Resource-leak diagnostics for stress tests.
    size_t debug_pool_blocks_allocated() const { return msg_pool_.blocks_allocated(); }
    size_t debug_interned_topics() const { return topic_intern_.topic_count(); }
    size_t debug_retained_count() const { return retained_.entry_count(); }
    uint32_t debug_retained_bytes() const { return retained_.bytes_used(); }
    uint32_t debug_global_tx_bytes() const { return global_logical_tx_; }
    size_t debug_trie_subscriptions() const { return topic_trie_.subscription_count(); }
    size_t debug_outbound_inflight_total() const;
    size_t debug_active_sessions() const;
    bool debug_any_will_scheduled() const;
#endif

private:
    struct ConnectionSlot {
        BrokerTransport transport;
        PacketParser *parser;
        TxQueue *tx;
        SessionHandle session;
        bool in_use;
        uint32_t attached_tick;
        uint8_t protocol_level;  // 4 = MQTT 3.1.1, 5 = MQTT 5 (set at CONNECT)
    };

    bool handle_packet(uint16_t transport_id, const ParsedPacket &pkt);
    void handle_connect(uint16_t transport_id, const ParsedPacket &pkt);
    void handle_publish(uint16_t transport_id, const ParsedPacket &pkt);
    void handle_puback(uint16_t transport_id, const ParsedPacket &pkt);
    void handle_pubrec(uint16_t transport_id, const ParsedPacket &pkt);
    void handle_pubrel(uint16_t transport_id, const ParsedPacket &pkt);
    void handle_pubcomp(uint16_t transport_id, const ParsedPacket &pkt);
    void handle_subscribe(uint16_t transport_id, const ParsedPacket &pkt);
    void handle_unsubscribe(uint16_t transport_id, const ParsedPacket &pkt);
    void handle_ping(uint16_t transport_id);
    void handle_disconnect(uint16_t transport_id, const ParsedPacket &pkt);

    void route_publish(uint16_t publisher_transport, const char *topic, uint8_t publish_qos,
                       bool retain, MessageHandle payload, size_t payload_len);
    void deliver_retained_for_filter(uint16_t transport_id, SessionHandle session,
                                     const char *filter, uint8_t options_byte);
    bool enqueue_delivery(uint16_t transport_id, SessionHandle session, const char *topic,
                          MessageHandle msg, size_t payload_len, uint8_t delivery_qos,
                          bool retain_flag);
    void disconnect_transport(uint16_t transport_id, bool slow_consumer);
    SessionRecord *session_record(SessionHandle h);
    SessionHandle session_for_transport(uint16_t transport_id) const;
    void touch_keepalive(uint16_t transport_id, uint32_t now_ticks);

    int16_t find_session_by_client_id(const char *client_id) const;
    uint16_t alloc_session_slot();
    void clear_session_state(uint16_t slot, bool clear_subs);
    void release_will(SessionRecord *s);
    void end_session(uint16_t slot, bool publish_will_now);
    void publish_stored_will(SessionRecord *s);
    void begin_offline_session(SessionRecord *s, uint32_t disconnect_expiry_override);
    void tick_offline_sessions();
    void send_connack(uint16_t transport_id, ReasonCode reason, bool session_present,
                      const char *assigned_client_id, uint16_t server_keep_alive);
    void send_disconnect(uint16_t transport_id, ReasonCode reason);
    void reject_connect(uint16_t transport_id, ReasonCode reason);
    bool session_has_presentable_state(uint16_t slot) const;
    bool enqueue_offline(uint16_t session_slot, const PendingDelivery &pd);
    void flush_offline_queue(uint16_t transport_id, SessionHandle session);
    bool write_bytes(uint16_t transport_id, const uint8_t *data, size_t len);
    bool send_puback(uint16_t transport_id, uint16_t packet_id, ReasonCode reason);
    bool send_pubrec(uint16_t transport_id, uint16_t packet_id, ReasonCode reason);
    bool send_pubrel(uint16_t transport_id, uint16_t packet_id, ReasonCode reason);
    bool send_pubcomp(uint16_t transport_id, uint16_t packet_id, ReasonCode reason);
    void clear_session_qos_state(uint16_t slot);
    void release_outbound_inflight(uint16_t session_slot, uint16_t packet_id);
    bool outbound_inflight_at_capacity(uint16_t session_slot) const;
    bool complete_tx_delivery(uint16_t transport_id, PendingDelivery *pd);
    void attach_publish_metadata(MessageHandle msg, PropertyHandle props, uint32_t publish_tick);
    bool message_expired(MessageHandle msg) const;
    void message_expiry_state(MessageHandle msg, bool *has_expiry, uint32_t *remaining) const;
    void retransmit_session_inflight(uint16_t transport_id, uint16_t slot);
    bool enqueue_retransmit(uint16_t transport_id, const OutboundInflightEntry &entry);
    void sweep_expired_retained();

    MessagePool msg_pool_;
    PropertyPool prop_pool_;
    TopicInternPool topic_intern_;
    TopicTrie topic_trie_;
    RetainedStore retained_;
    SessionRecord *sessions_;
    ConnectionSlot *connections_;
    OfflineQueue *offline_queues_;
    SessionOutboundInflight outbound_inflight_[BrokerLimits::MaxTcpClients];
    SessionInboundQoS2 inbound_qos2_[BrokerLimits::MaxTcpClients];
    uint32_t global_logical_tx_;
    uint32_t assign_serial_;

    uint32_t current_tick_;
    BrokerMetrics metrics_;
};

}  // namespace mqtt_broker

#endif