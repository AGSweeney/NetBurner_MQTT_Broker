#include "mqtt_broker/broker.hpp"

#include "mqtt_broker/broker_policy.hpp"
#include "mqtt_broker/debug_log.hpp"
#include "mqtt_broker/encode.hpp"
#include "mqtt_broker/protocol_error.hpp"
#include "mqtt_broker/subscription_options.hpp"
#include "mqtt_broker/topic_trie.hpp"

#include <cstdio>
#include <cstring>
#include <new>

// MQTT broker core — orchestrates client sessions, topic routing, and QoS
// delivery. Each connection owns a PacketParser and TxQueue; the platform
// drives on_readable() when data arrives, tick() for keepalive/session expiry,
// and drain_tx() to push queued PUBLISH frames via BrokerTransport callbacks
// (read/write/close_conn). Sessions may persist offline per MQTT 5 §3.1.2.5
// while subscriptions, inflight QoS state, and queued messages survive reconnect.

namespace mqtt_broker {

static void log_parser_error(uint16_t transport_id, const PacketParser *parser)
{
    if (parser == nullptr) {
        return;
    }

    const ParsedPacket &pkt = parser->packet();
    if (pkt.type == PacketType::Connect) {
        broker_debug_log(
            "[MQTT DBG] parse fail transport=%u packet=%s reason=0x%02X (%s) "
            "proto=%s lvl=%u flags=0x%02X client=%s will=%d",
            transport_id, packet_type_name(pkt.type),
            static_cast<unsigned>(parser->error_reason()),
            reason_code_name(parser->error_reason()), pkt.protocol_name, pkt.protocol_level,
            pkt.connect_flags, pkt.client_id, pkt.will_flag ? 1 : 0);
        return;
    }

    broker_debug_log("[MQTT DBG] parse fail transport=%u packet=%s reason=0x%02X (%s)",
                     transport_id, packet_type_name(pkt.type),
                     static_cast<unsigned>(parser->error_reason()),
                     reason_code_name(parser->error_reason()));
}

Broker::Broker()
    : msg_pool_({BrokerLimits::PayloadBlockSize, BrokerLimits::PayloadBlockCount,
                 BrokerLimits::PayloadSlotCount}),
      prop_pool_({BrokerLimits::MaxTcpClients, 16, 4096}),
      topic_intern_({BrokerLimits::MaxInternedTopics, BrokerLimits::MaxInternedBytes,
                     BrokerLimits::MaxTopicBytes}),
      topic_trie_({256, BrokerLimits::MaxSubsTotal}),
      retained_({BrokerLimits::MaxRetainedEntries, BrokerLimits::RetainedBytesTotal}),
      sessions_(nullptr),
      connections_(nullptr),
      offline_queues_(nullptr),
      global_logical_tx_(0),
      assign_serial_(0),
      current_tick_(0),
      metrics_{}
{
    sessions_ = new (std::nothrow) SessionRecord[BrokerLimits::MaxTcpClients];
    connections_ = new (std::nothrow) ConnectionSlot[BrokerLimits::MaxTcpClients];
    offline_queues_ = static_cast<OfflineQueue *>(::operator new(
        sizeof(OfflineQueue) * BrokerLimits::MaxTcpClients, std::nothrow));

    for (int i = 0; i < BrokerLimits::MaxTcpClients; ++i) {
        sessions_[i].self = {static_cast<uint16_t>(i), 1};
        sessions_[i].state = SessionState::Empty;
        sessions_[i].transport_id = 0xFFFF;
        sessions_[i].client_id[0] = '\0';
        sessions_[i].keep_alive_sec = 0;
        sessions_[i].client_max_packet_size = ConnackCaps::MaximumPacketSize;
        sessions_[i].client_receive_maximum = static_cast<uint16_t>(ConnackCaps::ReceiveMaximum);
        sessions_[i].last_control_tick = 0;
        sessions_[i].clean_start = true;
        sessions_[i].session_expiry_interval = 0;
        sessions_[i].session_expiry_deadline_tick = SESSION_TICK_NONE;
        sessions_[i].will_delay_interval = 0;
        sessions_[i].will_fire_deadline_tick = SESSION_TICK_NONE;
        sessions_[i].will = {};
        sessions_[i].suppress_will = false;
        sessions_[i].assign_serial = 0;
        sessions_[i].has_subscription_state = false;
        if (offline_queues_ != nullptr) {
            new (&offline_queues_[i])
                OfflineQueue({static_cast<size_t>(BrokerLimits::OfflineQueuePerClient)});
        }

        connections_[i].in_use = false;
        connections_[i].parser = nullptr;
        connections_[i].tx = nullptr;
        connections_[i].session = NULL_SESSION;
        connections_[i].transport = {};
        connections_[i].attached_tick = 0;
    }

    for (int i = 0; i < BrokerLimits::MaxTcpClients; ++i) {
        ParserLimits plimits = {BrokerLimits::MaxPacketBytes, BrokerLimits::MaxTopicBytes, 1024};
        connections_[i].parser = new (std::nothrow)
            PacketParser(plimits, &msg_pool_, &prop_pool_);
        connections_[i].tx = new (std::nothrow) TxQueue(
            {BrokerLimits::TxQueueDepthPerClient, BrokerLimits::TxLogicalPerClient});
    }
}

Broker::~Broker()
{
    for (int i = 0; i < BrokerLimits::MaxTcpClients; ++i) {
        delete connections_[i].parser;
        delete connections_[i].tx;
    }
    if (offline_queues_ != nullptr) {
        for (int i = 0; i < BrokerLimits::MaxTcpClients; ++i) {
            offline_queues_[i].~OfflineQueue();
        }
        ::operator delete(offline_queues_);
    }
    delete[] sessions_;
    delete[] connections_;
}

uint16_t Broker::attach_transport(const BrokerTransport &transport)
{
    for (int i = 0; i < BrokerLimits::MaxTcpClients; ++i) {
        if (!connections_[i].in_use) {
            connections_[i].in_use = true;
            connections_[i].transport = transport;
            connections_[i].transport.active = true;
            connections_[i].session = NULL_SESSION;
            if (connections_[i].parser != nullptr) {
                connections_[i].parser->reset();
            }
            connections_[i].attached_tick = current_tick_;
            return static_cast<uint16_t>(i);
        }
    }
    return 0xFFFF;
}

void Broker::detach_transport(uint16_t transport_id)
{
    if (transport_id >= BrokerLimits::MaxTcpClients) {
        return;
    }
    disconnect_transport(transport_id, false);
    connections_[transport_id].in_use = false;
    connections_[transport_id].transport.active = false;
}

SessionRecord *Broker::session_record(SessionHandle h)
{
    if (!session_handle_valid(h) || h.slot >= BrokerLimits::MaxTcpClients) {
        return nullptr;
    }
    SessionRecord &s = sessions_[h.slot];
    if (s.self.generation != h.generation) {
        return nullptr;
    }
    return &s;
}

SessionHandle Broker::session_for_transport(uint16_t transport_id) const
{
    if (transport_id >= BrokerLimits::MaxTcpClients || !connections_[transport_id].in_use) {
        return NULL_SESSION;
    }
    return connections_[transport_id].session;
}

#if defined(MQTT_BROKER_HOST_LE)
uint32_t Broker::debug_client_max_packet(uint16_t transport_id) const
{
    if (transport_id >= BrokerLimits::MaxTcpClients) {
        return 0;
    }
    return sessions_[transport_id].client_max_packet_size;
}
#endif

size_t Broker::connected_count() const
{
    size_t n = 0;
    for (int i = 0; i < BrokerLimits::MaxTcpClients; ++i) {
        if (sessions_[i].state == SessionState::Connected && sessions_[i].transport_id != 0xFFFF) {
            n++;
        }
    }
    return n;
}

bool Broker::transport_attached(uint16_t transport_id) const
{
    return transport_id < BrokerLimits::MaxTcpClients &&
           connections_[transport_id].in_use;
}

bool Broker::has_pending_tx() const
{
    for (int i = 0; i < BrokerLimits::MaxTcpClients; ++i) {
        if (connections_[i].in_use && connections_[i].tx != nullptr &&
            connections_[i].tx->count() > 0) {
            return true;
        }
    }
    return false;
}

void Broker::touch_keepalive(uint16_t transport_id, uint32_t now_ticks)
{
    SessionHandle h = session_for_transport(transport_id);
    SessionRecord *s = session_record(h);
    if (s != nullptr) {
        s->last_control_tick = now_ticks;
    }
}

void Broker::disconnect_transport(uint16_t transport_id, bool slow_consumer)
{
    if (transport_id >= BrokerLimits::MaxTcpClients || !connections_[transport_id].in_use) {
        return;
    }

    ConnectionSlot &c = connections_[transport_id];

    auto purge_tx_queue = [&]() {
        if (c.tx == nullptr) {
            return;
        }
        PendingDelivery pd;
        while (c.tx->peek(&pd)) {
            msg_pool_.msg_release(pd.message);
            topic_intern_.release(pd.topic);
            uint32_t released = 0;
            c.tx->pop(&released);
            if (global_logical_tx_ >= released) {
                global_logical_tx_ -= released;
            }
        }
    };

    purge_tx_queue();

    SessionRecord *s = session_record(c.session);
    if (s != nullptr && s->state == SessionState::Connected) {
        // QoS 1/2 inflight state intentionally survives the disconnect so the
        // messages can be retransmitted (DUP) if the session resumes. It is
        // released when the session itself ends (clean start, expiry, end_session).
        if (metrics_.clients_connected > 0) {
            metrics_.clients_connected--;
        }
        begin_offline_session(s, 0xFFFFFFFFu);
        s->transport_id = 0xFFFF;
    }
    c.session = NULL_SESSION;

    // A Will published during teardown (e.g. Session Expiry 0 ending the session
    // and routing the Will to this client's own matching subscription) can enqueue
    // into this slot's TX queue after the first purge. Purge again so nothing —
    // and no pooled message / interned topic reference — leaks into the next
    // client that reuses this connection slot.
    purge_tx_queue();

    if (slow_consumer) {
        metrics_.slow_consumer_disconnects++;
    }

    if (c.transport.ops.close_conn != nullptr && c.transport.active) {
        c.transport.ops.close_conn(c.transport.ctx);
        c.transport.active = false;
    }

    c.in_use = false;
}

int16_t Broker::find_session_by_client_id(const char *client_id) const
{
    if (client_id == nullptr || client_id[0] == '\0') {
        return -1;
    }
    for (int i = 0; i < BrokerLimits::MaxTcpClients; ++i) {
        if (sessions_[i].state != SessionState::Empty &&
            std::strcmp(sessions_[i].client_id, client_id) == 0) {
            return static_cast<int16_t>(i);
        }
    }
    return -1;
}

uint16_t Broker::alloc_session_slot()
{
    for (int i = 0; i < BrokerLimits::MaxTcpClients; ++i) {
        if (sessions_[i].state == SessionState::Empty) {
            return static_cast<uint16_t>(i);
        }
    }
    return 0xFFFF;
}

void Broker::release_will(SessionRecord *s)
{
    if (s == nullptr || !s->will.valid) {
        return;
    }
    if (msg_pool_.msg_valid(s->will.payload)) {
        msg_pool_.msg_release(s->will.payload);
    }
    s->will = {};
}

void Broker::clear_session_state(uint16_t slot, bool clear_subs)
{
    if (slot >= BrokerLimits::MaxTcpClients) {
        return;
    }
    SessionRecord &s = sessions_[slot];
    if (clear_subs) {
        topic_trie_.unsubscribe_all(slot);
        s.has_subscription_state = false;
    }
    if (offline_queues_ != nullptr) {
        PendingDelivery pd;
        while (offline_queues_[slot].peek(&pd)) {
            msg_pool_.msg_release(pd.message);
            topic_intern_.release(pd.topic);
            offline_queues_[slot].pop();
        }
    }
    release_will(&s);
    clear_session_qos_state(slot);
    s.session_expiry_deadline_tick = SESSION_TICK_NONE;
    s.will_fire_deadline_tick = SESSION_TICK_NONE;
    s.suppress_will = false;
}

void Broker::publish_stored_will(SessionRecord *s)
{
    if (s == nullptr || !s->will.valid || s->suppress_will || s->will.topic[0] == '\0') {
        return;
    }
    if (s->will.qos > 2) {
        return;
    }
    // Message expiry starts counting from publication of the will, not CONNECT.
    uint32_t will_expiry = msg_pool_.expiry_interval(s->will.payload);
    if (will_expiry > 0) {
        msg_pool_.set_expiry(s->will.payload, will_expiry, current_tick_);
    }
    route_publish(0xFFFF, s->will.topic, s->will.qos, s->will.retain, s->will.payload,
                  s->will.payload_len);
    metrics_.will_published++;
}

void Broker::end_session(uint16_t slot, bool publish_will_now)
{
    if (slot >= BrokerLimits::MaxTcpClients) {
        return;
    }
    SessionRecord &s = sessions_[slot];
    if (publish_will_now && !s.suppress_will) {
        publish_stored_will(&s);
    }
    clear_session_state(slot, true);
    s.state = SessionState::Empty;
    s.client_id[0] = '\0';
    s.keep_alive_sec = 0;
    s.transport_id = 0xFFFF;
    s.self.generation++;
    if (s.self.generation == 0) {
        s.self.generation = 1;
    }
}

void Broker::begin_offline_session(SessionRecord *s, uint32_t disconnect_expiry_override)
{
    if (s == nullptr) {
        return;
    }
    uint32_t expiry = s->session_expiry_interval;
    if (disconnect_expiry_override != 0xFFFFFFFFu) {
        expiry = disconnect_expiry_override;
    }

    // MQTT-5.0 §3.1.2.5.2 — Session Expiry Interval 0 ends the session on disconnect.
    if (expiry == 0) {
        end_session(s->self.slot, !s->suppress_will);
        return;
    }

    s->state = SessionState::Offline;
    s->session_expiry_deadline_tick = current_tick_ + expiry;
    if (!s->suppress_will && s->will.valid) {
        uint32_t delay = s->will_delay_interval;
        if (delay == 0 || delay >= expiry) {
            s->will_fire_deadline_tick = s->session_expiry_deadline_tick;
        } else {
            s->will_fire_deadline_tick = current_tick_ + delay;
        }
    }
}

void Broker::tick_offline_sessions()
{
    for (int i = 0; i < BrokerLimits::MaxTcpClients; ++i) {
        SessionRecord &s = sessions_[i];
        if (s.state != SessionState::Offline) {
            continue;
        }
        if (s.will_fire_deadline_tick != SESSION_TICK_NONE &&
            current_tick_ >= s.will_fire_deadline_tick) {
            publish_stored_will(&s);
            release_will(&s);
            s.will_fire_deadline_tick = SESSION_TICK_NONE;
        }
        if (s.session_expiry_deadline_tick != SESSION_TICK_NONE &&
            current_tick_ >= s.session_expiry_deadline_tick) {
            end_session(static_cast<uint16_t>(i), false);
        }
    }
}

void Broker::send_connack(uint16_t transport_id, ReasonCode reason, bool session_present,
                          const char *assigned_client_id, uint16_t server_keep_alive)
{
    uint8_t buf[192];
    size_t n = encode_connack(buf, sizeof(buf), reason, session_present, assigned_client_id,
                              server_keep_alive);
    if (n == 0 || transport_id >= BrokerLimits::MaxTcpClients ||
        connections_[transport_id].transport.ops.write == nullptr) {
        return;
    }
    int wrote = connections_[transport_id].transport.ops.write(
        connections_[transport_id].transport.ctx, buf, n);
    if (wrote < 0) {
        broker_debug_log("[MQTT DBG] CONNACK write error transport=%u bytes=%u rc=%d",
                         transport_id, static_cast<unsigned>(n), wrote);
        disconnect_transport(transport_id, false);
    } else if (wrote == 0) {
        broker_debug_log("[MQTT DBG] CONNACK write blocked transport=%u bytes=%u",
                         transport_id, static_cast<unsigned>(n));
    } else if (wrote != static_cast<int>(n)) {
        broker_debug_log("[MQTT DBG] CONNACK short write transport=%u bytes=%u rc=%d",
                         transport_id, static_cast<unsigned>(n), wrote);
        disconnect_transport(transport_id, false);
    }
}

void Broker::send_disconnect(uint16_t transport_id, ReasonCode reason)
{
    uint8_t buf[8];
    size_t n = encode_disconnect(buf, sizeof(buf), reason);
    if (n > 0 && transport_id < BrokerLimits::MaxTcpClients &&
        connections_[transport_id].transport.ops.write != nullptr) {
        connections_[transport_id].transport.ops.write(connections_[transport_id].transport.ctx,
                                                       buf, n);
    }
}

void Broker::reject_connect(uint16_t transport_id, ReasonCode reason)
{
    metrics_.connect_reject++;
    broker_debug_log("[MQTT DBG] CONNACK reject transport=%u reason=0x%02X (%s)",
                     transport_id, static_cast<unsigned>(reason), reason_code_name(reason));
    send_connack(transport_id, reason, false, nullptr, 0);
    if (transport_id < BrokerLimits::MaxTcpClients) {
        ConnectionSlot &c = connections_[transport_id];
        if (c.transport.ops.close_conn != nullptr && c.transport.active) {
            c.transport.ops.close_conn(c.transport.ctx);
            c.transport.active = false;
        }
        c.in_use = false;
        c.session = NULL_SESSION;
    }
}

bool Broker::session_has_presentable_state(uint16_t slot) const
{
    if (slot >= BrokerLimits::MaxTcpClients) {
        return false;
    }
    const SessionRecord &s = sessions_[slot];
    if (offline_queues_ != nullptr && offline_queues_[slot].count() > 0) {
        return true;
    }
    if (outbound_inflight_[slot].count() > 0) {
        return true;
    }
    return s.has_subscription_state;
}

void Broker::clear_session_qos_state(uint16_t slot)
{
    if (slot >= BrokerLimits::MaxTcpClients) {
        return;
    }
    outbound_inflight_[slot].clear(&msg_pool_, &topic_intern_);
    inbound_qos2_[slot].clear(&msg_pool_);
    outbound_inflight_[slot].reset_packet_ids();
}

bool Broker::write_bytes(uint16_t transport_id, const uint8_t *data, size_t len)
{
    if (transport_id >= BrokerLimits::MaxTcpClients || data == nullptr || len == 0 ||
        !connections_[transport_id].in_use ||
        connections_[transport_id].transport.ops.write == nullptr) {
        return false;
    }
    int rc = connections_[transport_id].transport.ops.write(
        connections_[transport_id].transport.ctx, data, len);
    return rc == static_cast<int>(len);
}

bool Broker::send_puback(uint16_t transport_id, uint16_t packet_id, ReasonCode reason)
{
    uint8_t buf[16];
    size_t n = encode_puback(buf, sizeof(buf), packet_id, reason);
    return n > 0 && write_bytes(transport_id, buf, n);
}

bool Broker::send_pubrec(uint16_t transport_id, uint16_t packet_id, ReasonCode reason)
{
    uint8_t buf[16];
    size_t n = encode_pubrec(buf, sizeof(buf), packet_id, reason);
    return n > 0 && write_bytes(transport_id, buf, n);
}

bool Broker::send_pubrel(uint16_t transport_id, uint16_t packet_id, ReasonCode reason)
{
    uint8_t buf[16];
    size_t n = encode_pubrel(buf, sizeof(buf), packet_id, reason);
    return n > 0 && write_bytes(transport_id, buf, n);
}

bool Broker::send_pubcomp(uint16_t transport_id, uint16_t packet_id, ReasonCode reason)
{
    uint8_t buf[16];
    size_t n = encode_pubcomp(buf, sizeof(buf), packet_id, reason);
    return n > 0 && write_bytes(transport_id, buf, n);
}

bool Broker::outbound_inflight_at_capacity(uint16_t session_slot) const
{
    if (session_slot >= BrokerLimits::MaxTcpClients) {
        return true;
    }
    if (sessions_[session_slot].state != SessionState::Connected) {
        return true;
    }
    const uint16_t max_inflight =
        effective_receive_maximum(sessions_[session_slot].client_receive_maximum);
    return outbound_inflight_[session_slot].count() >= max_inflight;
}

void Broker::release_outbound_inflight(uint16_t session_slot, uint16_t packet_id)
{
    if (session_slot >= BrokerLimits::MaxTcpClients) {
        return;
    }
    outbound_inflight_[session_slot].remove(packet_id, &msg_pool_, &topic_intern_);
}

bool Broker::complete_tx_delivery(uint16_t transport_id, PendingDelivery *pd)
{
    if (pd == nullptr || transport_id >= BrokerLimits::MaxTcpClients) {
        return false;
    }
    SessionRecord *s = session_record(connections_[transport_id].session);
    if (s == nullptr) {
        return false;
    }

    if (pd->delivery_qos == 0) {
        msg_pool_.msg_release(pd->message);
        topic_intern_.release(pd->topic);
        return true;
    }

    // Retransmissions already have an inflight entry holding its own references.
    if (outbound_inflight_[s->self.slot].find(pd->packet_id) != nullptr) {
        msg_pool_.msg_release(pd->message);
        topic_intern_.release(pd->topic);
        return true;
    }

    if (!outbound_inflight_[s->self.slot].insert(pd->packet_id, pd->message, pd->topic,
                                                 pd->delivery_qos, pd->retain_flag,
                                                 OutboundQoS2Phase::AwaitingPubrec)) {
        msg_pool_.msg_release(pd->message);
        topic_intern_.release(pd->topic);
        return true;
    }
    return true;
}

void Broker::attach_publish_metadata(MessageHandle msg, PropertyHandle props,
                                     uint32_t publish_tick)
{
    if (!msg_pool_.msg_valid(msg)) {
        return;
    }
    uint8_t buf[kMessagePropsCapacity];
    uint32_t expiry = 0;
    size_t len = serialize_forward_properties(prop_pool_, props, buf, sizeof(buf), &expiry);
    if (len > 0) {
        msg_pool_.set_props(msg, buf, len);
    }
    if (expiry > 0) {
        msg_pool_.set_expiry(msg, expiry, publish_tick);
    }
}

bool Broker::message_expired(MessageHandle msg) const
{
    uint32_t interval = msg_pool_.expiry_interval(msg);
    if (interval == 0) {
        return false;
    }
    return current_tick_ - msg_pool_.publish_tick(msg) >= interval;
}

void Broker::message_expiry_state(MessageHandle msg, bool *has_expiry, uint32_t *remaining) const
{
    uint32_t interval = msg_pool_.expiry_interval(msg);
    *has_expiry = interval > 0;
    *remaining = 0;
    if (interval > 0) {
        uint32_t elapsed = current_tick_ - msg_pool_.publish_tick(msg);
        // Forward at least 1 second so the on-wire value stays legal even if the
        // message expires between enqueue and drain (§3.3.2.3.3).
        *remaining = elapsed >= interval ? 1u : interval - elapsed;
    }
}

bool Broker::enqueue_offline(uint16_t session_slot, const PendingDelivery &pd)
{
    if (offline_queues_ == nullptr || session_slot >= BrokerLimits::MaxTcpClients) {
        return false;
    }
    PendingDelivery copy = pd;
    if (!msg_pool_.msg_acquire(copy.message)) {
        return false;
    }
    if (!topic_intern_.acquire(copy.topic)) {
        msg_pool_.msg_release(copy.message);
        return false;
    }
    if (!offline_queues_[session_slot].enqueue(copy)) {
        msg_pool_.msg_release(copy.message);
        topic_intern_.release(copy.topic);
        metrics_.dropped_quota++;
        metrics_.quota_drop_count++;
        return false;
    }
    return true;
}

void Broker::flush_offline_queue(uint16_t transport_id, SessionHandle session)
{
    if (offline_queues_ == nullptr) {
        return;
    }
    uint16_t slot = session.slot;
    if (slot >= BrokerLimits::MaxTcpClients) {
        return;
    }
    PendingDelivery pd;
    while (offline_queues_[slot].peek(&pd)) {
        if (message_expired(pd.message)) {
            metrics_.messages_expired++;
            msg_pool_.msg_release(pd.message);
            topic_intern_.release(pd.topic);
            offline_queues_[slot].pop();
            continue;
        }
        const char *topic = topic_intern_.c_str(pd.topic);
        size_t payload_len = msg_pool_.payload_size(pd.message);
        if (!enqueue_delivery(transport_id, session, topic, pd.message, payload_len, pd.delivery_qos,
                              pd.retain_flag)) {
            break;
        }
        offline_queues_[slot].pop();
    }
}

void Broker::retransmit_session_inflight(uint16_t transport_id, uint16_t slot)
{
    if (slot >= BrokerLimits::MaxTcpClients) {
        return;
    }
    for (size_t i = 0; i < SessionOutboundInflight::capacity(); ++i) {
        OutboundInflightEntry *e = outbound_inflight_[slot].entry_at(i);
        if (e == nullptr || !e->in_use) {
            continue;
        }
        if (e->delivery_qos == 2 && e->qos2_phase == OutboundQoS2Phase::AwaitingPubcomp) {
            // PUBLISH was acknowledged by PUBREC before the disconnect; only the
            // PUBREL needs to be repeated (MQTT 5 §4.4).
            send_pubrel(transport_id, e->packet_id, ReasonCode::Success);
            continue;
        }
        enqueue_retransmit(transport_id, *e);
    }
}

bool Broker::enqueue_retransmit(uint16_t transport_id, const OutboundInflightEntry &entry)
{
    if (transport_id >= BrokerLimits::MaxTcpClients || !connections_[transport_id].in_use) {
        return false;
    }
    TxQueue *tx = connections_[transport_id].tx;
    if (tx == nullptr || !msg_pool_.msg_valid(entry.message)) {
        return false;
    }

    const char *topic = topic_intern_.c_str(entry.topic);
    size_t payload_len = msg_pool_.payload_size(entry.message);
    size_t props_len = 0;
    msg_pool_.props(entry.message, &props_len);
    bool has_expiry = msg_pool_.expiry_interval(entry.message) > 0;
    size_t est = estimate_publish_size(topic, payload_len, entry.retain_flag, entry.delivery_qos,
                                       props_len, has_expiry);
    if (est == 0 ||
        tx->logical_bytes() + static_cast<uint32_t>(est) > BrokerLimits::TxLogicalPerClient ||
        global_logical_tx_ + static_cast<uint32_t>(est) >
            static_cast<uint32_t>(BrokerLimits::TxLogicalTotal)) {
        return false;
    }

    if (!msg_pool_.msg_acquire(entry.message)) {
        return false;
    }
    if (!topic_intern_.acquire(entry.topic)) {
        msg_pool_.msg_release(entry.message);
        return false;
    }

    PendingDelivery pd = {};
    pd.message = entry.message;
    pd.topic = entry.topic;
    pd.packet_id = entry.packet_id;
    pd.delivery_qos = entry.delivery_qos;
    pd.retain_flag = entry.retain_flag;
    pd.dup_flag = true;
    pd.encoded_offset = 0;
    pd.encoded_total_size = static_cast<uint32_t>(est);

    if (!tx->enqueue(pd, static_cast<uint32_t>(est))) {
        msg_pool_.msg_release(entry.message);
        topic_intern_.release(entry.topic);
        return false;
    }
    global_logical_tx_ += static_cast<uint32_t>(est);
    metrics_.publish_sent++;
    return true;
}

bool Broker::enqueue_delivery(uint16_t transport_id, SessionHandle session, const char *topic,
                              MessageHandle msg, size_t payload_len, uint8_t delivery_qos,
                              bool retain_flag)
{
    if (transport_id >= BrokerLimits::MaxTcpClients || !connections_[transport_id].in_use) {
        return false;
    }
    SessionRecord *s = session_record(session);
    if (s == nullptr || s->state != SessionState::Connected) {
        return false;
    }
    if (delivery_qos > ConnackCaps::MaximumQos) {
        delivery_qos = ConnackCaps::MaximumQos;
    }

    if (message_expired(msg)) {
        metrics_.messages_expired++;
        return false;
    }

    uint16_t packet_id = 0;
    if (delivery_qos > 0) {
        if (outbound_inflight_at_capacity(s->self.slot)) {
            metrics_.dropped_quota++;
            return false;
        }
        packet_id = outbound_inflight_[s->self.slot].next_packet_id();
        if (packet_id == 0) {
            metrics_.dropped_quota++;
            return false;
        }
    }

    size_t props_len = 0;
    msg_pool_.props(msg, &props_len);
    bool has_expiry = msg_pool_.expiry_interval(msg) > 0;
    size_t est =
        estimate_publish_size(topic, payload_len, retain_flag, delivery_qos, props_len, has_expiry);
    if (est > s->client_max_packet_size) {
        metrics_.dropped_too_large++;
        return false;
    }

    TxQueue *tx = connections_[transport_id].tx;
    if (tx == nullptr) {
        return false;
    }

    if (tx->logical_bytes() + static_cast<uint32_t>(est) > BrokerLimits::TxLogicalPerClient ||
        global_logical_tx_ + static_cast<uint32_t>(est) >
            static_cast<uint32_t>(BrokerLimits::TxLogicalTotal)) {
        disconnect_transport(transport_id, true);
        return false;
    }

    TopicHandle th = topic_intern_.intern(topic);
    if (!topic_intern_.valid(th)) {
        return false;
    }

    PendingDelivery pd = {};
    pd.message = msg;
    pd.topic = th;
    pd.packet_id = packet_id;
    pd.delivery_qos = delivery_qos;
    pd.retain_flag = retain_flag;
    pd.dup_flag = false;
    pd.encoded_offset = 0;
    pd.encoded_total_size = static_cast<uint32_t>(est);

    if (!msg_pool_.msg_acquire(msg)) {
        topic_intern_.release(th);
        return false;
    }

    if (!tx->enqueue(pd, static_cast<uint32_t>(est))) {
        msg_pool_.msg_release(msg);
        topic_intern_.release(th);
        disconnect_transport(transport_id, true);
        return false;
    }

    global_logical_tx_ += static_cast<uint32_t>(est);
    if (global_logical_tx_ > metrics_.tx_logical_high_water) {
        metrics_.tx_logical_high_water = global_logical_tx_;
    }
    metrics_.publish_sent++;
    return true;
}

void Broker::route_publish(uint16_t publisher_transport, const char *topic, uint8_t publish_qos,
                           bool retain, MessageHandle payload, size_t payload_len)
{
    // Hold one reference for the duration of routing so the handle stays valid
    // for retained_.put / .remove; release it before returning so route_publish
    // never leaks an intern reference.
    TopicHandle th = topic_intern_.intern(topic);
    if (!topic_intern_.valid(th)) {
        return;
    }

    if (retain) {
        if (payload_len == 0) {
            // Zero-length retained payload deletes the retained message (MQTT 5 §3.3.1.3).
            retained_.remove(th, &msg_pool_, &topic_intern_);
        } else if (!retained_.put(th, payload, &msg_pool_, &topic_intern_)) {
            metrics_.pool_exhaustion++;
        }
    }

    TopicSubscription matches[BrokerLimits::MaxSubsPerClient * 2];
    size_t match_count = topic_trie_.match(topic, matches, BrokerLimits::MaxSubsPerClient * 2);
    if (match_count == 0) {
        topic_intern_.release(th);
        return;
    }

    SessionHandle pub_session = session_for_transport(publisher_transport);
    SessionRecord *pub = session_record(pub_session);
    uint16_t pub_id = pub != nullptr ? pub->self.slot : 0xFFFF;

    uint16_t seen_sessions[BrokerLimits::MaxTcpClients];
    size_t seen_count = 0;

    for (size_t i = 0; i < match_count; ++i) {
        uint16_t sid = matches[i].session_id;
        bool already = false;
        for (size_t j = 0; j < seen_count; ++j) {
            if (seen_sessions[j] == sid) {
                already = true;
                break;
            }
        }
        if (already) {
            continue;
        }
        seen_sessions[seen_count++] = sid;

        SubscriptionMatch sm[BrokerLimits::MaxSubsPerClient];
        size_t sm_count = 0;
        for (size_t j = 0; j < match_count && sm_count < BrokerLimits::MaxSubsPerClient; ++j) {
            if (matches[j].session_id != sid) {
                continue;
            }
            SubscriptionMatch &m = sm[sm_count++];
            m.session_id = sid;
            m.max_qos = matches[j].max_qos;
            m.options.max_qos = matches[j].max_qos;
            m.options.no_local = (matches[j].options_byte >> 2) & 0x01u;
            m.options.retain_as_published = (matches[j].options_byte >> 3) & 0x01u;
            m.options.retain_handling = (matches[j].options_byte >> 4) & 0x03u;
        }

        DeliveryPlan plan = compute_delivery_plan(publish_qos, pub_id, sm, sm_count);
        if (!plan.deliver) {
            continue;
        }

        SessionRecord *sub = session_record({sid, sessions_[sid].self.generation});
        if (sub == nullptr) {
            continue;
        }

        bool out_retain = retain && plan.retain_as_published;

        if (sub->state == SessionState::Offline && plan.effective_qos > 0) {
            TopicHandle th_copy = topic_intern_.intern(topic);
            if (!topic_intern_.valid(th_copy)) {
                continue;
            }
            if (!msg_pool_.msg_acquire(payload)) {
                topic_intern_.release(th_copy);
                continue;
            }
            PendingDelivery offline_pd = {};
            offline_pd.message = payload;
            offline_pd.topic = th_copy;
            offline_pd.delivery_qos = plan.effective_qos;
            offline_pd.retain_flag = out_retain;
            if (!enqueue_offline(sid, offline_pd)) {
                msg_pool_.msg_release(payload);
                topic_intern_.release(th_copy);
            }
            continue;
        }

        if (sub->state != SessionState::Connected) {
            continue;
        }

        enqueue_delivery(sub->transport_id, sub->self, topic, payload, payload_len,
                         plan.effective_qos, out_retain);
    }

    topic_intern_.release(th);
}

// Authenticates the client, allocates or resumes a session slot, and negotiates
// CONNECT properties (keep-alive, max packet size, receive maximum). Handles
// session takeover (§3.1.4), clean start vs session present (§3.1.2.5), and
// stores the will message for later publication.
void Broker::handle_connect(uint16_t transport_id, const ParsedPacket &pkt)
{
    auto release_pkt_will = [&](const ParsedPacket &p) {
        if (msg_pool_.msg_valid(p.will_payload)) {
            msg_pool_.msg_release(p.will_payload);
        }
    };

    AuthCheckFn auth = broker_auth_handler();
    if (auth != nullptr) {
        ReasonCode auth_rc = auth(pkt.client_id, pkt.has_username ? pkt.username : nullptr,
                                  pkt.has_password ? pkt.password : nullptr, pkt.password_len);
        if (auth_rc != ReasonCode::Success) {
            release_pkt_will(pkt);
            reject_connect(transport_id, auth_rc);
            return;
        }
    }

    char assigned_id[64] = {};
    const char *connack_id = nullptr;
    bool server_assigned = false;

    if (pkt.client_id[0] == '\0') {
        if (!pkt.clean_start) {
            release_pkt_will(pkt);
            reject_connect(transport_id, ReasonCode::ProtocolError);
            return;
        }
        uint16_t slot_hint = alloc_session_slot();
        if (slot_hint == 0xFFFF) {
            release_pkt_will(pkt);
            reject_connect(transport_id, ReasonCode::QuotaExceeded);
            return;
        }
        assign_serial_++;
        snprintf(assigned_id, sizeof(assigned_id), "nb-%u-%lu", slot_hint,
                 static_cast<unsigned long>(assign_serial_));
        connack_id = assigned_id;
        server_assigned = true;
    }

    const char *effective_id = server_assigned ? assigned_id : pkt.client_id;
    int16_t existing = find_session_by_client_id(effective_id);
    uint16_t slot = existing >= 0 ? static_cast<uint16_t>(existing) : alloc_session_slot();
    if (slot == 0xFFFF) {
        release_pkt_will(pkt);
        reject_connect(transport_id, ReasonCode::QuotaExceeded);
        return;
    }

    if (pkt.will_flag && pkt.will_topic[0] == '\0') {
        release_pkt_will(pkt);
        reject_connect(transport_id, ReasonCode::TopicNameInvalid);
        return;
    }

    SessionRecord *s = &sessions_[slot];
    uint16_t old_transport = 0xFFFF;
    if (existing >= 0 && s->state == SessionState::Connected && s->transport_id != 0xFFFF) {
        old_transport = s->transport_id;
    }

    // §3.1.4-3 Session takeover: DISCONNECT 0x8E to the old transport, then close it.
    // Will on takeover follows §3.1.3-9 and §3.1.3.2.2 (non-normative): with Clean Start 0
    // and Will Delay > 0 the Will MUST NOT be sent; with Will Delay 0 or Clean Start 1
    // the Will is published before the session continues or ends.
    if (old_transport != 0xFFFF && old_transport != transport_id) {
        const bool resume_session = !pkt.clean_start && !server_assigned;
        const uint32_t prev_will_delay = s->will_delay_interval;
        const bool had_will = s->will.valid && s->will.topic[0] != '\0';

        if (had_will && (!resume_session || prev_will_delay == 0)) {
            publish_stored_will(s);
            release_will(s);
        }

        send_disconnect(old_transport, ReasonCode::SessionTakenOver);
        metrics_.session_takeovers++;
        ConnectionSlot &old_c = connections_[old_transport];
        if (old_c.transport.ops.close_conn != nullptr && old_c.transport.active) {
            old_c.transport.ops.close_conn(old_c.transport.ctx);
            old_c.transport.active = false;
        }
        old_c.session = NULL_SESSION;
        old_c.in_use = false;
        if (metrics_.clients_connected > 0) {
            metrics_.clients_connected--;
        }
    }

    if (pkt.clean_start || server_assigned) {
        clear_session_state(slot, true);
        clear_session_qos_state(slot);
    }

    std::strncpy(s->client_id, effective_id, sizeof(s->client_id) - 1);
    s->client_id[sizeof(s->client_id) - 1] = '\0';
    uint16_t negotiated_keepalive = pkt.keep_alive;
    const int max_keep_alive = broker_policy().max_keep_alive_sec;
    if (max_keep_alive > 0) {
        if (negotiated_keepalive == 0 ||
            negotiated_keepalive > static_cast<uint16_t>(max_keep_alive)) {
            negotiated_keepalive = static_cast<uint16_t>(max_keep_alive);
        }
    }
    s->keep_alive_sec = negotiated_keepalive;
    s->client_max_packet_size = pkt.client_max_packet_size;
    s->client_receive_maximum = pkt.client_receive_maximum;
    s->clean_start = pkt.clean_start || server_assigned;
    s->session_expiry_interval = pkt.session_expiry_interval;
    s->will_delay_interval = pkt.will_delay_interval;
    s->session_expiry_deadline_tick = SESSION_TICK_NONE;
    s->will_fire_deadline_tick = SESSION_TICK_NONE;
    s->suppress_will = false;

    if (s->state == SessionState::Offline) {
        s->will_fire_deadline_tick = SESSION_TICK_NONE;
        s->session_expiry_deadline_tick = SESSION_TICK_NONE;
    }

    if (pkt.will_flag) {
        release_will(s);
        s->will.valid = true;
        std::strncpy(s->will.topic, pkt.will_topic, sizeof(s->will.topic) - 1);
        s->will.qos = pkt.will_qos;
        s->will.retain = pkt.will_retain;
        s->will.payload = pkt.will_payload;
        s->will.payload_len = pkt.will_payload_len;
        // Forwardable will properties + expiry ride on the pooled message; the
        // expiry clock starts when the will is published (publish_stored_will).
        attach_publish_metadata(s->will.payload, pkt.will_properties, 0);
    }

    bool session_present =
        !pkt.clean_start && !server_assigned && session_has_presentable_state(slot);

    s->state = SessionState::Connected;
    s->transport_id = transport_id;
    s->last_control_tick = current_tick_;
    connections_[transport_id].session = {slot, s->self.generation};

    metrics_.connect_accept++;
    metrics_.clients_connected++;
    if (metrics_.clients_connected > metrics_.clients_connected_peak) {
        metrics_.clients_connected_peak = metrics_.clients_connected;
    }

    uint16_t server_keep_alive = 0;
    if (negotiated_keepalive != pkt.keep_alive) {
        server_keep_alive = negotiated_keepalive;
    }

    send_connack(transport_id, ReasonCode::Success, session_present, connack_id,
                 server_keep_alive);
    if (!connections_[transport_id].in_use) {
        return;
    }
    broker_debug_log(
        "[MQTT DBG] CONNECT ok transport=%u client=%s proto=%s lvl=%u clean=%d "
        "keepalive=%u negotiated=%u session_present=%d",
        transport_id, s->client_id, pkt.protocol_name, pkt.protocol_level,
        pkt.clean_start ? 1 : 0, pkt.keep_alive, negotiated_keepalive,
        session_present ? 1 : 0);
    if (session_present) {
        retransmit_session_inflight(transport_id, slot);
    }
    flush_offline_queue(transport_id, {slot, s->self.generation});
}

void Broker::deliver_retained_for_filter(uint16_t transport_id, SessionHandle session,
                                         const char *filter, uint8_t options_byte)
{
    SessionRecord *sub = session_record(session);
    if (sub == nullptr || sub->state != SessionState::Connected || filter == nullptr) {
        return;
    }

    uint8_t rh = (options_byte >> 4) & 0x03u;
    if (rh == 2) {
        return;
    }

    for (size_t ri = 0; ri < static_cast<size_t>(BrokerLimits::MaxRetainedEntries); ++ri) {
        TopicHandle th = NULL_TOPIC;
        MessageHandle msg = NULL_MESSAGE;
        if (!retained_.entry_at(ri, &th, &msg)) {
            continue;
        }
        const char *topic = topic_intern_.c_str(th);
        if (topic[0] == '\0' || !TopicTrie::topic_matches_filter(topic, filter)) {
            continue;
        }
        if (message_expired(msg)) {
            metrics_.messages_expired++;
            retained_.remove(th, &msg_pool_, &topic_intern_);
            continue;
        }
        size_t payload_len = msg_pool_.payload_size(msg);
        uint8_t req_qos = options_byte & 0x03u;
        uint8_t granted = req_qos > ConnackCaps::MaximumQos ? ConnackCaps::MaximumQos : req_qos;
        // Messages sent due to a new subscription always carry RETAIN=1 (MQTT 5 §3.3.1.3).
        enqueue_delivery(transport_id, session, topic, msg, payload_len, granted, true);
    }
}

// Validates publish QoS, attaches forwardable properties/expiry, and routes the
// message to matching subscribers. QoS 0 routes immediately; QoS 1 routes then
// PUBACKs; QoS 2 holds the publish until PUBREL (§4.3.3) before routing.
void Broker::handle_publish(uint16_t transport_id, const ParsedPacket &pkt)
{
    if (pkt.qos > ConnackCaps::MaximumQos) {
        disconnect_transport(transport_id, false);
        return;
    }

    metrics_.publish_received++;

    SessionRecord *s = session_record(session_for_transport(transport_id));
    if (s == nullptr) {
        if (msg_pool_.msg_valid(pkt.payload)) {
            msg_pool_.msg_release(pkt.payload);
        }
        return;
    }

    attach_publish_metadata(pkt.payload, pkt.properties, current_tick_);

    if (pkt.qos == 0) {
        route_publish(transport_id, pkt.topic, pkt.qos, pkt.retain, pkt.payload, pkt.payload_len);
        if (msg_pool_.msg_valid(pkt.payload)) {
            msg_pool_.msg_release(pkt.payload);
        }
        return;
    }

    if (pkt.qos == 1) {
        route_publish(transport_id, pkt.topic, pkt.qos, pkt.retain, pkt.payload, pkt.payload_len);
        send_puback(transport_id, pkt.packet_id, ReasonCode::Success);
        if (msg_pool_.msg_valid(pkt.payload)) {
            msg_pool_.msg_release(pkt.payload);
        }
        return;
    }

    InboundQoS2Entry *existing = inbound_qos2_[s->self.slot].find(pkt.packet_id);
    if (existing != nullptr && existing->pubrec_sent) {
        if (msg_pool_.msg_valid(pkt.payload)) {
            msg_pool_.msg_release(pkt.payload);
        }
        send_pubrec(transport_id, pkt.packet_id, ReasonCode::Success);
        return;
    }

    if (!msg_pool_.msg_acquire(pkt.payload)) {
        disconnect_transport(transport_id, false);
        return;
    }

    InboundQoS2Entry *pending = inbound_qos2_[s->self.slot].insert(
        pkt.packet_id, pkt.topic, pkt.payload, pkt.payload_len, pkt.retain, pkt.qos, pkt.dup);
    if (pending == nullptr) {
        msg_pool_.msg_release(pkt.payload);
        disconnect_transport(transport_id, false);
        return;
    }

    if (!pending->pubrec_sent) {
        pending->pubrec_sent = true;
        send_pubrec(transport_id, pkt.packet_id, ReasonCode::Success);
    }
}

void Broker::handle_puback(uint16_t transport_id, const ParsedPacket &pkt)
{
    SessionRecord *s = session_record(session_for_transport(transport_id));
    if (s == nullptr || pkt.packet_id == 0) {
        return;
    }
    OutboundInflightEntry *entry = outbound_inflight_[s->self.slot].find(pkt.packet_id);
    if (entry == nullptr || entry->delivery_qos != 1) {
        return;
    }
    release_outbound_inflight(s->self.slot, pkt.packet_id);
}

void Broker::handle_pubrec(uint16_t transport_id, const ParsedPacket &pkt)
{
    SessionRecord *s = session_record(session_for_transport(transport_id));
    if (s == nullptr || pkt.packet_id == 0) {
        return;
    }
    OutboundInflightEntry *entry = outbound_inflight_[s->self.slot].find(pkt.packet_id);
    if (entry == nullptr || entry->delivery_qos != 2 ||
        entry->qos2_phase != OutboundQoS2Phase::AwaitingPubrec) {
        return;
    }
    if (pkt.ack_reason_code >= 0x80) {
        // Client rejected the publish; abandon the QoS 2 exchange (MQTT 5 §4.3.3).
        release_outbound_inflight(s->self.slot, pkt.packet_id);
        return;
    }
    entry->qos2_phase = OutboundQoS2Phase::AwaitingPubcomp;
    send_pubrel(transport_id, pkt.packet_id, ReasonCode::Success);
}

void Broker::handle_pubrel(uint16_t transport_id, const ParsedPacket &pkt)
{
    SessionRecord *s = session_record(session_for_transport(transport_id));
    if (s == nullptr || pkt.packet_id == 0) {
        return;
    }

    InboundQoS2Entry *pending = inbound_qos2_[s->self.slot].find(pkt.packet_id);
    if (pending == nullptr) {
        return;
    }

    route_publish(transport_id, pending->topic, pending->qos, pending->retain, pending->payload,
                  pending->payload_len);
    inbound_qos2_[s->self.slot].remove(pkt.packet_id, &msg_pool_);
    send_pubcomp(transport_id, pkt.packet_id, ReasonCode::Success);
}

void Broker::handle_pubcomp(uint16_t transport_id, const ParsedPacket &pkt)
{
    SessionRecord *s = session_record(session_for_transport(transport_id));
    if (s == nullptr || pkt.packet_id == 0) {
        return;
    }
    OutboundInflightEntry *entry = outbound_inflight_[s->self.slot].find(pkt.packet_id);
    if (entry == nullptr || entry->delivery_qos != 2) {
        return;
    }
    release_outbound_inflight(s->self.slot, pkt.packet_id);
}

// Adds topic filters to the trie, returns granted QoS in SUBACK (§3.8.3), and
// delivers matching retained messages per subscription options (Retain Handling
// §3.8.2.1.3). Shared subscriptions ($share/...) are rejected.
void Broker::handle_subscribe(uint16_t transport_id, const ParsedPacket &pkt)
{
    SessionRecord *s = session_record(session_for_transport(transport_id));
    if (s == nullptr) {
        return;
    }

    ReasonCode rcs[kMaxSubscribeEntries];
    for (size_t i = 0; i < pkt.subscribe_count; ++i) {
        const ParsedSubscribeEntry &e = pkt.subscribe_entries[i];
        if (std::strncmp(e.filter, "$share/", 7) == 0) {
            rcs[i] = ReasonCode::SharedSubscriptionsNotSupported;
            continue;
        }
        uint8_t req_qos = e.options_byte & 0x03u;
        uint8_t granted = req_qos > ConnackCaps::MaximumQos ? ConnackCaps::MaximumQos : req_qos;
        uint16_t sub_id = 0;
        bool existed = false;
        if (!topic_trie_.subscribe(e.filter, s->self.slot, granted, e.options_byte, &sub_id,
                                   &existed)) {
            rcs[i] = ReasonCode::QuotaExceeded;
        } else {
            rcs[i] = static_cast<ReasonCode>(granted);
            s->has_subscription_state = true;
            // Retain Handling 1: send retained only if the subscription did not already exist.
            uint8_t rh = (e.options_byte >> 4) & 0x03u;
            if (!(rh == 1 && existed)) {
                deliver_retained_for_filter(transport_id, s->self, e.filter, e.options_byte);
            }
        }
    }

    uint8_t buf[64];
    size_t n = encode_suback(buf, sizeof(buf), pkt.packet_id, rcs, pkt.subscribe_count);
    if (n > 0 && connections_[transport_id].transport.ops.write != nullptr) {
        connections_[transport_id].transport.ops.write(connections_[transport_id].transport.ctx,
                                                       buf, n);
    }
}

void Broker::handle_unsubscribe(uint16_t transport_id, const ParsedPacket &pkt)
{
    SessionRecord *s = session_record(session_for_transport(transport_id));
    if (s == nullptr || pkt.subscribe_count == 0) {
        return;
    }

    ReasonCode rcs[kMaxSubscribeEntries];
    for (size_t i = 0; i < pkt.subscribe_count; ++i) {
        rcs[i] = topic_trie_.unsubscribe(pkt.subscribe_entries[i].filter, s->self.slot)
                     ? ReasonCode::Success
                     : ReasonCode::NoSubscriptionExisted;
    }

    uint8_t buf[64];
    size_t n = encode_unsuback(buf, sizeof(buf), pkt.packet_id, rcs, pkt.subscribe_count);
    if (n > 0 && connections_[transport_id].transport.ops.write != nullptr) {
        connections_[transport_id].transport.ops.write(connections_[transport_id].transport.ctx,
                                                       buf, n);
    }
}

void Broker::handle_ping(uint16_t transport_id)
{
    uint8_t buf[4];
    size_t n = encode_pingresp(buf, sizeof(buf));
    if (n > 0 && connections_[transport_id].transport.ops.write != nullptr) {
        connections_[transport_id].transport.ops.write(connections_[transport_id].transport.ctx,
                                                       buf, n);
    }
}

// Graceful client shutdown: publishes or suppresses the will per reason code
// (§3.14.4), optionally overrides Session Expiry Interval, then moves the
// session offline (or ends it if expiry is 0 — see begin_offline_session).
void Broker::handle_disconnect(uint16_t transport_id, const ParsedPacket &pkt)
{
    SessionRecord *s = session_record(session_for_transport(transport_id));
    if (s != nullptr) {
        // §3.14.4: only reason 0x04 (Disconnect with Will Message) publishes the
        // Will on a graceful client DISCONNECT. Reason 0x00 and other graceful
        // reasons suppress it. Abrupt loss and keep-alive timeout remain separate.
        const bool publish_will_on_disconnect = (pkt.disconnect_reason == 0x04);
        s->suppress_will = !publish_will_on_disconnect;
        if (pkt.disconnect_session_expiry != 0xFFFFFFFFu) {
            s->session_expiry_interval = pkt.disconnect_session_expiry;
        }
        if (publish_will_on_disconnect) {
            broker_debug_log("[MQTT DBG] DISCONNECT reason=0x04 transport=%u (will publish)",
                             transport_id);
        }
    }
    disconnect_transport(transport_id, false);
}

bool Broker::handle_packet(uint16_t transport_id, const ParsedPacket &pkt)
{
    touch_keepalive(transport_id, current_tick_);

    switch (pkt.type) {
    case PacketType::Connect:
        handle_connect(transport_id, pkt);
        return true;
    case PacketType::Publish:
        handle_publish(transport_id, pkt);
        return true;
    case PacketType::Puback:
        handle_puback(transport_id, pkt);
        return true;
    case PacketType::Pubrec:
        handle_pubrec(transport_id, pkt);
        return true;
    case PacketType::Pubrel:
        handle_pubrel(transport_id, pkt);
        return true;
    case PacketType::Pubcomp:
        handle_pubcomp(transport_id, pkt);
        return true;
    case PacketType::Subscribe:
        handle_subscribe(transport_id, pkt);
        return true;
    case PacketType::Unsubscribe:
        handle_unsubscribe(transport_id, pkt);
        return true;
    case PacketType::Pingreq:
        handle_ping(transport_id);
        return true;
    case PacketType::Disconnect:
        handle_disconnect(transport_id, pkt);
        return true;
    case PacketType::Auth:
        // Enhanced authentication is not advertised in CONNACK (§3.2.2.3.11).
        send_disconnect(transport_id, ReasonCode::BadAuthenticationMethod);
        disconnect_transport(transport_id, false);
        return false;
    default: {
        ProtocolErrorAction act = protocol_error_action(ReasonCode::ProtocolError);
        metrics_.parser_errors++;
        broker_debug_log("[MQTT DBG] unsupported packet transport=%u type=%s",
                         transport_id, packet_type_name(pkt.type));
        if (act.disconnect) {
            disconnect_transport(transport_id, false);
        }
        return false;
    }
    }
}

void Broker::on_readable(uint16_t transport_id)
{
    if (transport_id >= BrokerLimits::MaxTcpClients || !connections_[transport_id].in_use) {
        return;
    }

    ConnectionSlot &c = connections_[transport_id];
    if (c.parser == nullptr || c.transport.ops.read == nullptr) {
        return;
    }

    uint8_t buf[BrokerLimits::ParserReadChunk];
    int n = c.transport.ops.read(c.transport.ctx, buf, sizeof(buf));
    if (n < 0) {
        if (n != -1) {
            broker_debug_log("[MQTT DBG] read error transport=%u rc=%d", transport_id, n);
            disconnect_transport(transport_id, false);
        }
        return;
    }
    if (n == 0) {
        broker_debug_log("[MQTT DBG] peer closed transport=%u", transport_id);
        detach_transport(transport_id);
        return;
    }

    size_t off = 0;
    while (off < static_cast<size_t>(n)) {
        size_t consumed = c.parser->feed(buf + off, static_cast<size_t>(n) - off);
        if (consumed == 0) {
            break;
        }
        off += consumed;

        if (c.parser->packet_ready()) {
            handle_packet(transport_id, c.parser->packet());
            if (prop_pool_.valid(c.parser->packet().properties)) {
                prop_pool_.release(c.parser->packet().properties);
            }
            if (prop_pool_.valid(c.parser->packet().will_properties)) {
                prop_pool_.release(c.parser->packet().will_properties);
            }
            c.parser->reset();
        } else if (c.parser->phase() == ParsePhase::Error) {
            metrics_.parser_errors++;
            log_parser_error(transport_id, c.parser);
            disconnect_transport(transport_id, false);
            break;
        }
    }
}

void Broker::drain_tx(uint16_t transport_id)
{
    if (transport_id >= BrokerLimits::MaxTcpClients || !connections_[transport_id].in_use) {
        return;
    }

    ConnectionSlot &c = connections_[transport_id];
    if (c.tx == nullptr || c.transport.ops.write == nullptr) {
        return;
    }

    PendingDelivery pd;
    while (c.tx->peek(&pd)) {
        const char *topic = topic_intern_.c_str(pd.topic);
        size_t payload_len = msg_pool_.payload_size(pd.message);
        bool has_expiry = false;
        uint32_t expiry_remaining = 0;
        message_expiry_state(pd.message, &has_expiry, &expiry_remaining);
        uint8_t out[2048];
        size_t wrote = encode_publish(out, sizeof(out), topic, pd.retain_flag, pd.delivery_qos,
                                      pd.packet_id, pd.dup_flag, &msg_pool_, pd.message,
                                      payload_len, pd.encoded_offset, has_expiry,
                                      expiry_remaining);
        if (wrote == 0) {
            uint32_t released = 0;
            complete_tx_delivery(transport_id, &pd);
            c.tx->pop(&released);
            if (global_logical_tx_ >= released) {
                global_logical_tx_ -= released;
            }
            break;
        }

        int rc = c.transport.ops.write(c.transport.ctx, out, wrote);
        if (rc < 0) {
            broker_debug_log("[MQTT DBG] publish write error transport=%u rc=%d", transport_id,
                             rc);
            disconnect_transport(transport_id, true);
            break;
        }
        if (rc == 0) {
            break;
        }

        c.tx->advance_offset(static_cast<uint32_t>(rc));
        pd.encoded_offset += static_cast<uint32_t>(rc);

        if (pd.encoded_offset >= pd.encoded_total_size) {
            uint32_t released = 0;
            complete_tx_delivery(transport_id, &pd);
            c.tx->pop(&released);
            if (global_logical_tx_ >= released) {
                global_logical_tx_ -= released;
            }
        } else {
            break;
        }
    }
}

void Broker::sweep_expired_retained()
{
    for (size_t ri = 0; ri < static_cast<size_t>(BrokerLimits::MaxRetainedEntries); ++ri) {
        TopicHandle th = NULL_TOPIC;
        MessageHandle msg = NULL_MESSAGE;
        if (!retained_.entry_at(ri, &th, &msg)) {
            continue;
        }
        if (message_expired(msg)) {
            metrics_.messages_expired++;
            retained_.remove(th, &msg_pool_, &topic_intern_);
        }
    }
}

void Broker::tick(uint32_t now_ticks)
{
    current_tick_ = now_ticks;
    tick_offline_sessions();
    sweep_expired_retained();
    for (int i = 0; i < BrokerLimits::MaxTcpClients; ++i) {
        if (!connections_[i].in_use) {
            continue;
        }

        SessionRecord *s = session_record(connections_[i].session);
        if (s == nullptr || s->state != SessionState::Connected) {
            const int connect_timeout = broker_policy().connect_handshake_timeout_sec;
            if (connect_timeout > 0 && connections_[i].attached_tick != 0) {
                uint32_t pending = now_ticks - connections_[i].attached_tick;
                if (pending > static_cast<uint32_t>(connect_timeout)) {
                    broker_debug_log("[MQTT DBG] connect timeout transport=%u after %lus", i,
                                     static_cast<unsigned long>(pending));
                    disconnect_transport(static_cast<uint16_t>(i), false);
                    connections_[i].in_use = false;
                    connections_[i].transport.active = false;
                }
            }
            continue;
        }

        if (s->keep_alive_sec == 0) {
            continue;
        }

        uint32_t elapsed = now_ticks - s->last_control_tick;
        uint32_t threshold =
            static_cast<uint32_t>(s->keep_alive_sec) * BrokerLimits::KeepAliveMultiplierTenths / 10u;
        if (elapsed > threshold) {
            metrics_.keepalive_disconnects++;
            disconnect_transport(static_cast<uint16_t>(i), false);
        }
    }
}

#if defined(MQTT_BROKER_HOST_LE)
int16_t Broker::debug_find_session(const char *client_id) const
{
    return find_session_by_client_id(client_id);
}

bool Broker::debug_session_present(const char *client_id) const
{
    int16_t slot = find_session_by_client_id(client_id);
    if (slot < 0) {
        return false;
    }
    return session_has_presentable_state(static_cast<uint16_t>(slot));
}

uint32_t Broker::debug_offline_queue_depth(uint16_t session_slot) const
{
    if (offline_queues_ == nullptr || session_slot >= BrokerLimits::MaxTcpClients) {
        return 0;
    }
    return static_cast<uint32_t>(offline_queues_[session_slot].count());
}

bool Broker::debug_enqueue_offline(uint16_t session_slot, const PendingDelivery &pd)
{
    return enqueue_offline(session_slot, pd);
}

bool Broker::debug_fill_offline_queue(uint16_t session_slot, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        MessageHandle h = msg_pool_.acquire_empty();
        if (!message_handle_valid(h)) {
            return false;
        }
        uint8_t b = static_cast<uint8_t>('q');
        if (!msg_pool_.append_payload(h, &b, 1)) {
            msg_pool_.msg_release(h);
            return false;
        }
        TopicHandle th = topic_intern_.intern("offline/q");
        PendingDelivery pd = {};
        pd.message = h;
        pd.topic = th;
        pd.delivery_qos = 0;
        if (!enqueue_offline(session_slot, pd)) {
            msg_pool_.msg_release(h);
            return false;
        }
    }
    return true;
}

size_t Broker::debug_outbound_inflight_total() const
{
    size_t n = 0;
    for (int i = 0; i < BrokerLimits::MaxTcpClients; ++i) {
        n += outbound_inflight_[i].count();
    }
    return n;
}

size_t Broker::debug_active_sessions() const
{
    size_t n = 0;
    for (int i = 0; i < BrokerLimits::MaxTcpClients; ++i) {
        if (sessions_[i].state != SessionState::Empty) {
            n++;
        }
    }
    return n;
}

bool Broker::debug_any_will_scheduled() const
{
    for (int i = 0; i < BrokerLimits::MaxTcpClients; ++i) {
        if (sessions_[i].state != SessionState::Empty &&
            sessions_[i].will_fire_deadline_tick != SESSION_TICK_NONE) {
            return true;
        }
    }
    return false;
}
#endif

}  // namespace mqtt_broker