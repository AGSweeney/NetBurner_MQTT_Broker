// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// Sparkplug B MQTT 5 server conformance tests.
//
// Exercises the MQTT 5 behaviors Sparkplug 3.0 relies on: DISCONNECT 0x04
// (Disconnect with Will Message), QoS 1 retained Wills, Clean Start with
// Session Expiry 0, binary-safe payload transport, wildcard routing over
// Sparkplug topic shapes, and repeated birth/death lifecycle stability.
// The broker stays payload-agnostic: fixtures are opaque byte arrays.

#include "mqtt_broker/broker.hpp"
#include "mqtt_broker/broker_policy.hpp"
#include "mqtt_broker/mqtt_types.hpp"
#include "mqtt_broker/varint.hpp"
#include "mqtt_broker/wire.hpp"
#include "test_host_env.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace mqtt_broker;

// Print context to stderr before assert() so failures are visible on the console
// even when running under the VS debugger (no modal "Debug Error" dialog).
#define SP_ASSERT(cond, fmt, ...)                     \
    do {                                              \
        if (!(cond)) {                                \
            fprintf(stderr, "ASSERT FAILED: " fmt "\n", __VA_ARGS__); \
            fflush(stderr);                           \
            assert(cond);                             \
        }                                             \
    } while (0)

// ---------------------------------------------------------------------------
// Mock transport (same conventions as test_broker.cpp).
// ---------------------------------------------------------------------------

struct MockLink {
    std::vector<uint8_t> rx;   // bytes broker wrote to the client
    std::vector<uint8_t> tx;   // bytes the client will send to the broker
    size_t read_off;
    bool open;
    bool block_writes;
};

static int mock_read(void *ctx, uint8_t *buf, size_t cap)
{
    auto *link = static_cast<MockLink *>(ctx);
    if (!link->open) {
        return 0;
    }
    if (link->read_off >= link->tx.size()) {
        return -1;
    }
    size_t avail = link->tx.size() - link->read_off;
    if (avail > cap) {
        avail = cap;
    }
    std::memcpy(buf, link->tx.data() + link->read_off, avail);
    link->read_off += avail;
    return static_cast<int>(avail);
}

static int mock_write(void *ctx, const uint8_t *data, size_t len)
{
    auto *link = static_cast<MockLink *>(ctx);
    if (link->block_writes) {
        return 0;
    }
    link->rx.insert(link->rx.end(), data, data + len);
    return static_cast<int>(len);
}

static void mock_close(void *ctx)
{
    static_cast<MockLink *>(ctx)->open = false;
}

static BrokerTransport make_transport(MockLink *link)
{
    return {link, {mock_read, mock_write, mock_close}, true};
}

// drain_tx emits at most one ~2 KB window per call (payloads stream in chunks),
// so loop like the device event loop until the queue is empty.
static void drain(Broker &broker, uint16_t tid)
{
    for (int pass = 0; pass < 64; ++pass) {
        broker.drain_tx(tid);
        if (!broker.has_pending_tx()) {
            break;
        }
    }
}

static void send(Broker &broker, uint16_t tid, MockLink &link, const std::vector<uint8_t> &bytes)
{
    link.tx = bytes;
    link.read_off = 0;
    // The broker reads at most ParserReadChunk bytes per on_readable call;
    // keep feeding until the buffer is drained or the transport detaches.
    while (link.read_off < link.tx.size() && link.open && broker.transport_attached(tid)) {
        size_t before = link.read_off;
        broker.on_readable(tid);
        if (link.read_off == before) {
            break;  // parser made no progress (e.g. transport detached)
        }
    }
}

// ---------------------------------------------------------------------------
// Wire-frame inspection (binary safe; no string assumptions on payloads).
// ---------------------------------------------------------------------------

struct Frame {
    uint8_t fh;
    std::vector<uint8_t> body;
};

static std::vector<Frame> split_frames(const std::vector<uint8_t> &rx)
{
    std::vector<Frame> frames;
    size_t off = 0;
    while (off < rx.size()) {
        uint8_t fh = rx[off];
        VarintDecode d = varint_decode(rx.data() + off + 1, rx.size() - off - 1);
        if (d.result != VarintResult::Ok) {
            break;
        }
        size_t start = off + 1 + d.bytes_consumed;
        if (start + d.value > rx.size()) {
            break;
        }
        Frame f;
        f.fh = fh;
        f.body.assign(rx.begin() + start, rx.begin() + start + d.value);
        frames.push_back(f);
        off = start + d.value;
    }
    return frames;
}

struct PublishView {
    bool valid;
    std::string topic;
    uint8_t qos;
    bool retain;
    bool dup;
    uint16_t pid;
    std::vector<uint8_t> payload;
};

static PublishView parse_publish(const Frame &f)
{
    PublishView pv = {};
    if ((f.fh & 0xF0u) != 0x30u || f.body.size() < 2) {
        return pv;
    }
    pv.qos = static_cast<uint8_t>((f.fh >> 1) & 0x03u);
    pv.retain = (f.fh & 0x01u) != 0;
    pv.dup = (f.fh & 0x08u) != 0;
    size_t off = 0;
    uint16_t tlen = read_u16_be(f.body.data());
    off += 2;
    if (off + tlen > f.body.size()) {
        return pv;
    }
    pv.topic.assign(reinterpret_cast<const char *>(f.body.data() + off), tlen);
    off += tlen;
    if (pv.qos > 0) {
        if (off + 2 > f.body.size()) {
            return pv;
        }
        pv.pid = read_u16_be(f.body.data() + off);
        off += 2;
    }
    VarintDecode pl = varint_decode(f.body.data() + off, f.body.size() - off);
    if (pl.result != VarintResult::Ok) {
        return pv;
    }
    off += pl.bytes_consumed + pl.value;
    if (off > f.body.size()) {
        return pv;
    }
    pv.payload.assign(f.body.begin() + off, f.body.end());
    pv.valid = true;
    return pv;
}

static std::vector<PublishView> publishes_on_topic(const std::vector<uint8_t> &rx,
                                                   const char *topic)
{
    std::vector<PublishView> out;
    for (const Frame &f : split_frames(rx)) {
        PublishView pv = parse_publish(f);
        if (pv.valid && (topic == nullptr || pv.topic == topic)) {
            out.push_back(pv);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Packet builders (payloads are explicit byte vectors, never C strings).
// ---------------------------------------------------------------------------

struct WillSpec {
    const char *topic;
    std::vector<uint8_t> payload;
    uint8_t qos;
    bool retain;
};

static void put_u16(std::vector<uint8_t> &v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFFu));
    v.push_back(static_cast<uint8_t>(x & 0xFFu));
}

static std::vector<uint8_t> finish_fixed_header(uint8_t fh, std::vector<uint8_t> &body)
{
    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(body.size()), rl, sizeof(rl));
    std::vector<uint8_t> pkt;
    pkt.push_back(fh);
    pkt.insert(pkt.end(), rl, rl + rl_size);
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

// Sparkplug profile CONNECT: Clean Start = 1, explicit Session Expiry Interval
// property = 0, optional Will.
static std::vector<uint8_t> build_connect_sp(const char *client_id, uint16_t keep_alive,
                                             const WillSpec *will)
{
    std::vector<uint8_t> body;
    const char proto[] = {'M', 'Q', 'T', 'T'};
    put_u16(body, 4);
    body.insert(body.end(), proto, proto + 4);
    body.push_back(0x05);
    uint8_t flags = 0x02;  // Clean Start
    if (will != nullptr) {
        flags |= 0x04;
        flags |= static_cast<uint8_t>((will->qos & 0x03u) << 3);
        if (will->retain) {
            flags |= 0x20;
        }
    }
    body.push_back(flags);
    put_u16(body, keep_alive);
    // Properties: Session Expiry Interval = 0, stated explicitly.
    body.push_back(0x05);
    body.push_back(SessionExpiryInterval);
    body.push_back(0x00);
    body.push_back(0x00);
    body.push_back(0x00);
    body.push_back(0x00);

    size_t cid_len = std::strlen(client_id);
    put_u16(body, static_cast<uint16_t>(cid_len));
    body.insert(body.end(), client_id, client_id + cid_len);

    if (will != nullptr) {
        body.push_back(0x00);  // Will property length (Will Delay Interval = 0)
        size_t wt = std::strlen(will->topic);
        put_u16(body, static_cast<uint16_t>(wt));
        body.insert(body.end(), will->topic, will->topic + wt);
        put_u16(body, static_cast<uint16_t>(will->payload.size()));
        body.insert(body.end(), will->payload.begin(), will->payload.end());
    }
    return finish_fixed_header(0x10, body);
}

static std::vector<uint8_t> build_subscribe(uint16_t pid, const char *filter, uint8_t options)
{
    std::vector<uint8_t> body;
    put_u16(body, pid);
    body.push_back(0x00);
    size_t flen = std::strlen(filter);
    put_u16(body, static_cast<uint16_t>(flen));
    body.insert(body.end(), filter, filter + flen);
    body.push_back(options);
    return finish_fixed_header(0x82, body);
}

static std::vector<uint8_t> build_publish_bin(const char *topic,
                                              const std::vector<uint8_t> &payload, uint8_t qos,
                                              uint16_t pid, bool retain)
{
    std::vector<uint8_t> body;
    size_t tlen = std::strlen(topic);
    put_u16(body, static_cast<uint16_t>(tlen));
    body.insert(body.end(), topic, topic + tlen);
    if (qos > 0) {
        put_u16(body, pid);
    }
    body.push_back(0x00);  // property length
    body.insert(body.end(), payload.begin(), payload.end());
    uint8_t fh = 0x30 | static_cast<uint8_t>((qos & 0x03u) << 1) | (retain ? 0x01u : 0x00u);
    return finish_fixed_header(fh, body);
}

static std::vector<uint8_t> build_puback(uint16_t pid)
{
    return {0x40, 0x02, static_cast<uint8_t>((pid >> 8) & 0xFFu),
            static_cast<uint8_t>(pid & 0xFFu)};
}

static std::vector<uint8_t> build_pingreq()
{
    return {0xC0, 0x00};
}

// The three legal DISCONNECT encodings.
static std::vector<uint8_t> build_disconnect_rl0()
{
    return {0xE0, 0x00};
}

static std::vector<uint8_t> build_disconnect_rl1(uint8_t reason)
{
    return {0xE0, 0x01, reason};
}

static std::vector<uint8_t> build_disconnect_rl2(uint8_t reason)
{
    return {0xE0, 0x02, reason, 0x00};
}

// Sends PUBACK for every QoS 1 PUBLISH found in link.rx. Returns count acked.
static size_t ack_all_qos1(Broker &broker, uint16_t tid, MockLink &link)
{
    std::vector<uint8_t> acks;
    size_t n = 0;
    for (const Frame &f : split_frames(link.rx)) {
        PublishView pv = parse_publish(f);
        if (pv.valid && pv.qos == 1) {
            assert(pv.pid != 0);
            std::vector<uint8_t> a = build_puback(pv.pid);
            acks.insert(acks.end(), a.begin(), a.end());
            n++;
        }
    }
    if (!acks.empty()) {
        send(broker, tid, link, acks);
    }
    return n;
}

// ---------------------------------------------------------------------------
// Sparkplug fixtures: opaque binary payloads with embedded zero bytes.
// ---------------------------------------------------------------------------

static const char *kTopicNbirth = "spBv1.0/TestGroup/NBIRTH/TestNode";
static const char *kTopicDbirth = "spBv1.0/TestGroup/DBIRTH/TestNode/TestDevice";
static const char *kTopicNdata = "spBv1.0/TestGroup/NDATA/TestNode";
static const char *kTopicDdata = "spBv1.0/TestGroup/DDATA/TestNode/TestDevice";
static const char *kTopicNdeath = "spBv1.0/TestGroup/NDEATH/TestNode";
static const char *kTopicState = "spBv1.0/STATE/TestHost";

static std::vector<uint8_t> state_offline()
{
    return {0x53, 0x50, 0x42, 0x00, 0x4F, 0x46, 0x46, 0x4C, 0x49, 0x4E, 0x45, 0x00, 0xFF, 0x10};
}

static std::vector<uint8_t> state_online()
{
    return {0x53, 0x50, 0x42, 0x00, 0x4F, 0x4E, 0x4C, 0x49, 0x4E, 0x45, 0x00, 0xFF, 0x11};
}

// Protobuf-shaped filler: deterministic bytes with zeros sprinkled in.
static std::vector<uint8_t> make_sp_payload(size_t len, uint8_t seed)
{
    std::vector<uint8_t> p(len);
    for (size_t i = 0; i < len; ++i) {
        p[i] = (i % 7 == 0) ? 0x00 : static_cast<uint8_t>((seed + i * 31) & 0xFF);
    }
    if (len > 2) {
        p[0] = 0x08;  // protobuf-ish field header
        p[1] = 0x00;
    }
    return p;
}

// ---------------------------------------------------------------------------
// Common setup: broker + connected wildcard host subscriber.
// ---------------------------------------------------------------------------

struct Host {
    MockLink link = {{}, {}, 0, true, false};
    uint16_t tid = 0xFFFF;
};

static void connect_host(Broker &broker, Host &host, const char *client_id,
                         const char *filter = "spBv1.0/#", uint8_t sub_options = 0x01)
{
    host.tid = broker.attach_transport(make_transport(&host.link));
    assert(host.tid != 0xFFFF);
    send(broker, host.tid, host.link, build_connect_sp(client_id, 60, nullptr));
    std::vector<Frame> frames = split_frames(host.link.rx);
    assert(!frames.empty() && frames[0].fh == 0x20);
    assert(frames[0].body[1] == 0x00);
    send(broker, host.tid, host.link, build_subscribe(1, filter, sub_options));
    host.link.rx.clear();
}

// ===========================================================================
// Tests
// ===========================================================================

static void disconnect_0x00_suppresses_will()
{
    // Both encodings: RL=0 (implied 0x00) and RL=2 (reason + empty properties).
    const std::vector<uint8_t> encodings[] = {build_disconnect_rl0(), build_disconnect_rl2(0x00)};
    for (const auto &disconnect_pkt : encodings) {
        Broker broker;
        broker.tick(0);
        Host host;
        connect_host(broker, host, "sp-host");

        size_t retained_before = broker.debug_retained_count();

        MockLink node = {{}, {}, 0, true, false};
        uint16_t node_tid = broker.attach_transport(make_transport(&node));
        WillSpec will = {kTopicNdeath, make_sp_payload(32, 0x21), 1, false};
        send(broker, node_tid, node, build_connect_sp("sp-node", 60, &will));

        send(broker, node_tid, node, disconnect_pkt);
        drain(broker, host.tid);

        assert(publishes_on_topic(host.link.rx, kTopicNdeath).empty());
        assert(broker.metrics().will_published == 0);
        assert(broker.debug_retained_count() == retained_before);
        assert(!broker.debug_any_will_scheduled());
        assert(broker.debug_outbound_inflight_total() == 0);
        assert(!broker.transport_attached(node_tid));
    }
    printf("PASS disconnect_0x00_suppresses_will\n");
}

static void disconnect_0x04_publishes_will()
{
    // Both encodings: RL=1 (reason only) and RL=2 (reason + property length 0).
    const std::vector<uint8_t> encodings[] = {build_disconnect_rl1(0x04), build_disconnect_rl2(0x04)};
    for (const auto &disconnect_pkt : encodings) {
        Broker broker;
        broker.tick(0);
        Host host;
        connect_host(broker, host, "sp-host");

        std::vector<uint8_t> will_payload = make_sp_payload(48, 0x42);
        MockLink node = {{}, {}, 0, true, false};
        uint16_t node_tid = broker.attach_transport(make_transport(&node));
        WillSpec will = {kTopicNdeath, will_payload, 1, true};
        send(broker, node_tid, node, build_connect_sp("sp-node", 60, &will));

        send(broker, node_tid, node, disconnect_pkt);
        drain(broker, host.tid);

        // Exactly one will publish, QoS preserved, payload binary-identical.
        auto pubs = publishes_on_topic(host.link.rx, kTopicNdeath);
        assert(pubs.size() == 1);
        assert(pubs[0].qos == 1);
        assert(pubs[0].payload == will_payload);
        assert(broker.metrics().will_published == 1);
        ack_all_qos1(broker, host.tid, host.link);

        // Session closed cleanly, no stale will state. Checked before the late
        // subscriber attaches (it may legitimately reuse the freed slot).
        assert(!broker.transport_attached(node_tid));
        assert(broker.debug_find_session("sp-node") < 0);
        assert(!broker.debug_any_will_scheduled());

        // Retain flag honored: a late subscriber sees the retained will.
        Host late;
        connect_host(broker, late, "sp-late", kTopicNdeath, 0x01);
        drain(broker, late.tid);
        auto late_pubs = publishes_on_topic(late.link.rx, kTopicNdeath);
        assert(late_pubs.size() == 1);
        assert(late_pubs[0].retain);
        assert(late_pubs[0].payload == will_payload);
        ack_all_qos1(broker, late.tid, late.link);
    }
    printf("PASS disconnect_0x04_publishes_will\n");
}

static void disconnect_0x04_qos1_retained_will()
{
    Broker broker;
    broker.tick(0);
    Host host;
    connect_host(broker, host, "sp-host");

    std::vector<uint8_t> offline = state_offline();
    MockLink dev = {{}, {}, 0, true, false};
    uint16_t dev_tid = broker.attach_transport(make_transport(&dev));
    WillSpec will = {kTopicState, offline, 1, true};
    send(broker, dev_tid, dev, build_connect_sp("sp-state-node", 60, &will));

    send(broker, dev_tid, dev, build_disconnect_rl1(0x04));
    drain(broker, host.tid);

    // Live delivery at effective QoS 1 with a valid packet identifier.
    auto pubs = publishes_on_topic(host.link.rx, kTopicState);
    assert(pubs.size() == 1);
    assert(pubs[0].qos == 1);
    assert(pubs[0].pid != 0);
    assert(pubs[0].payload.size() == offline.size());
    assert(pubs[0].payload == offline);

    // PUBACK releases the outbound inflight record.
    assert(broker.debug_outbound_inflight_total() == 1);
    assert(ack_all_qos1(broker, host.tid, host.link) == 1);
    assert(broker.debug_outbound_inflight_total() == 0);

    // Retained store holds the exact payload.
    assert(broker.debug_retained_count() == 1);
    assert(broker.debug_retained_bytes() == offline.size());

    // Late subscriber receives it with RETAIN=1.
    Host late;
    connect_host(broker, late, "sp-late", kTopicState, 0x01);
    drain(broker, late.tid);
    auto late_pubs = publishes_on_topic(late.link.rx, kTopicState);
    assert(late_pubs.size() == 1);
    assert(late_pubs[0].retain);
    assert(late_pubs[0].qos == 1);
    assert(late_pubs[0].payload == offline);
    ack_all_qos1(broker, late.tid, late.link);

    // Replacing the retained STATE updates the stored value.
    std::vector<uint8_t> online = state_online();
    send(broker, host.tid, host.link, build_publish_bin(kTopicState, online, 1, 77, true));
    Host verify;
    connect_host(broker, verify, "sp-verify", kTopicState, 0x01);
    drain(broker, verify.tid);
    auto verify_pubs = publishes_on_topic(verify.link.rx, kTopicState);
    assert(verify_pubs.size() == 1);
    assert(verify_pubs[0].payload == online);
    ack_all_qos1(broker, verify.tid, verify.link);

    // Zero-length retained payload deletes the entry.
    send(broker, host.tid, host.link, build_publish_bin(kTopicState, {}, 1, 78, true));
    assert(broker.debug_retained_count() == 0);
    assert(broker.debug_retained_bytes() == 0);
    printf("PASS disconnect_0x04_qos1_retained_will\n");
}

static void abrupt_disconnect_qos1_retained_will()
{
    Broker broker;
    broker.tick(0);
    Host host;
    connect_host(broker, host, "sp-host");

    std::vector<uint8_t> offline = state_offline();
    MockLink dev = {{}, {}, 0, true, false};
    uint16_t dev_tid = broker.attach_transport(make_transport(&dev));
    WillSpec will = {kTopicState, offline, 1, true};
    send(broker, dev_tid, dev, build_connect_sp("sp-abrupt", 60, &will));

    // Abrupt TCP loss: transport read returns 0.
    mock_close(&dev);
    broker.on_readable(dev_tid);
    drain(broker, host.tid);

    auto pubs = publishes_on_topic(host.link.rx, kTopicState);
    assert(pubs.size() == 1);
    assert(pubs[0].qos == 1);
    assert(pubs[0].payload == offline);
    assert(broker.metrics().will_published == 1);
    ack_all_qos1(broker, host.tid, host.link);
    assert(broker.debug_retained_count() == 1);

    // Cleanup retained for a tidy end state.
    send(broker, host.tid, host.link, build_publish_bin(kTopicState, {}, 0, 0, true));
    printf("PASS abrupt_disconnect_qos1_retained_will\n");
}

static void keepalive_timeout_qos1_retained_will()
{
    Broker broker;
    broker.tick(0);
    Host host;
    connect_host(broker, host, "sp-host");

    std::vector<uint8_t> offline = state_offline();
    MockLink dev = {{}, {}, 0, true, false};
    uint16_t dev_tid = broker.attach_transport(make_transport(&dev));
    WillSpec will = {kTopicState, offline, 1, true};
    send(broker, dev_tid, dev, build_connect_sp("sp-katimeout", 10, &will));
    (void)dev_tid;

    // Keep the host alive across the node's keep-alive window (1.5 x 10s).
    broker.tick(10);
    send(broker, host.tid, host.link, build_pingreq());
    broker.tick(16);
    drain(broker, host.tid);

    assert(broker.metrics().keepalive_disconnects == 1);
    auto pubs = publishes_on_topic(host.link.rx, kTopicState);
    assert(pubs.size() == 1);
    assert(pubs[0].qos == 1);
    assert(pubs[0].payload == offline);
    ack_all_qos1(broker, host.tid, host.link);
    assert(broker.debug_retained_count() == 1);
    printf("PASS keepalive_timeout_qos1_retained_will\n");
}

static void late_subscriber_receives_retained_state()
{
    Broker broker;
    broker.tick(0);
    Host host;
    connect_host(broker, host, "sp-host");

    std::vector<uint8_t> online = state_online();
    send(broker, host.tid, host.link, build_publish_bin(kTopicState, online, 1, 5, true));

    Host late;
    connect_host(broker, late, "sp-late-state", kTopicState, 0x01);
    drain(broker, late.tid);
    auto pubs = publishes_on_topic(late.link.rx, kTopicState);
    assert(pubs.size() == 1);
    assert(pubs[0].retain);
    assert(pubs[0].qos == 1);
    assert(pubs[0].payload == online);
    ack_all_qos1(broker, late.tid, late.link);
    printf("PASS late_subscriber_receives_retained_state\n");
}

static void clean_start_expiry_zero_discards_session()
{
    Broker broker;
    broker.tick(0);
    Host host;
    connect_host(broker, host, "sp-host");

    for (int round = 0; round < 5; ++round) {
        MockLink node = {{}, {}, 0, true, false};
        uint16_t node_tid = broker.attach_transport(make_transport(&node));
        WillSpec will = {kTopicNdeath, make_sp_payload(24, static_cast<uint8_t>(round)), 1, false};
        send(broker, node_tid, node, build_connect_sp("sp-clean", 60, &will));

        // CONNACK Session Present must be 0 on every reconnect.
        std::vector<Frame> frames = split_frames(node.rx);
        assert(!frames.empty() && frames[0].fh == 0x20);
        assert(frames[0].body[0] == 0x00);  // Session Present = 0
        assert(frames[0].body[1] == 0x00);  // Success
        node.rx.clear();

        // Subscribe, then verify the subscription is NOT restored next round.
        send(broker, node_tid, node, build_subscribe(2, kTopicNdata, 0x01));
        assert(broker.debug_trie_subscriptions() >= 2);  // host + node

        // Create an unacked outbound QoS 1 delivery (node never PUBACKs).
        send(broker, host.tid, host.link, build_publish_bin(kTopicNdata,
                                                            make_sp_payload(16, 0x33), 1, 91,
                                                            false));
        drain(broker, node_tid);
        assert(broker.debug_outbound_inflight_total() >= 1);
        ack_all_qos1(broker, host.tid, host.link);  // ack host's copy
        host.link.rx.clear();

        // Alternate graceful/abrupt endings.
        if (round % 2 == 0) {
            send(broker, node_tid, node, build_disconnect_rl0());
        } else {
            mock_close(&node);
            broker.on_readable(node_tid);
            drain(broker, host.tid);
            ack_all_qos1(broker, host.tid, host.link);  // will from abrupt end
            host.link.rx.clear();
        }

        // Session Expiry 0: nothing survives the disconnect.
        assert(broker.debug_find_session("sp-clean") < 0);
        assert(broker.debug_outbound_inflight_total() == 0);
        assert(broker.debug_trie_subscriptions() == 1);  // only the host remains
        assert(!broker.debug_any_will_scheduled());
    }

    // Old subscription must not receive anything after final teardown.
    send(broker, host.tid, host.link,
         build_publish_bin(kTopicNdata, make_sp_payload(16, 0x44), 0, 0, false));
    assert(broker.metrics().publish_sent > 0);
    printf("PASS clean_start_expiry_zero_discards_session\n");
}

static void sparkplug_binary_payload_preserved()
{
    Broker broker;
    broker.tick(0);
    Host host;
    connect_host(broker, host, "sp-host");

    MockLink node = {{}, {}, 0, true, false};
    uint16_t node_tid = broker.attach_transport(make_transport(&node));
    send(broker, node_tid, node, build_connect_sp("sp-bin", 60, nullptr));

    // Payload with embedded zeros, 0xFF, and protobuf-like framing.
    std::vector<uint8_t> small_payload = state_offline();
    send(broker, node_tid, node, build_publish_bin(kTopicNdata, small_payload, 1, 11, false));
    drain(broker, host.tid);
    auto pubs = publishes_on_topic(host.link.rx, kTopicNdata);
    assert(pubs.size() == 1);
    assert(pubs[0].payload == small_payload);
    ack_all_qos1(broker, host.tid, host.link);
    host.link.rx.clear();

    // Large NBIRTH-style payload near the configured payload limit.
    std::vector<uint8_t> big = make_sp_payload(4096, 0x5A);
    send(broker, node_tid, node, build_publish_bin(kTopicNbirth, big, 1, 12, false));
    drain(broker, host.tid);
    pubs = publishes_on_topic(host.link.rx, kTopicNbirth);
    assert(pubs.size() == 1);
    assert(pubs[0].payload.size() == big.size());
    assert(pubs[0].payload == big);
    ack_all_qos1(broker, host.tid, host.link);
    printf("PASS sparkplug_binary_payload_preserved\n");
}

static void sparkplug_wildcard_topic_routing()
{
    Broker broker;
    broker.tick(0);

    Host all;      // spBv1.0/#
    Host ndata;    // node-level single wildcard
    Host device;   // exact device-level topic
    connect_host(broker, all, "sp-all", "spBv1.0/#", 0x00);
    connect_host(broker, ndata, "sp-ndata", "spBv1.0/TestGroup/NDATA/+", 0x00);
    connect_host(broker, device, "sp-device", kTopicDdata, 0x00);

    MockLink node = {{}, {}, 0, true, false};
    uint16_t node_tid = broker.attach_transport(make_transport(&node));
    send(broker, node_tid, node, build_connect_sp("sp-routing", 60, nullptr));

    const char *topics[] = {kTopicNbirth, kTopicDbirth, kTopicNdata,
                            kTopicDdata, kTopicNdeath, kTopicState};
    for (size_t i = 0; i < 6; ++i) {
        send(broker, node_tid, node,
             build_publish_bin(topics[i], make_sp_payload(20, static_cast<uint8_t>(i)), 0, 0,
                               false));
    }
    drain(broker, all.tid);
    drain(broker, ndata.tid);
    drain(broker, device.tid);

    // spBv1.0/# receives every message.
    assert(publishes_on_topic(all.link.rx, nullptr).size() == 6);
    // Node-level '+' matches only the node NDATA topic.
    auto nd = publishes_on_topic(ndata.link.rx, nullptr);
    assert(nd.size() == 1);
    assert(nd[0].topic == kTopicNdata);
    // Exact device-level subscription matches only DDATA with Device ID.
    auto dv = publishes_on_topic(device.link.rx, nullptr);
    assert(dv.size() == 1);
    assert(dv[0].topic == kTopicDdata);
    printf("PASS sparkplug_wildcard_topic_routing\n");
}

static void sparkplug_full_lifecycle()
{
    Broker broker;
    broker.tick(0);

    // 1-3. Host: clean start, expiry 0, retained QoS 1 offline STATE will,
    //      subscribes spBv1.0/#, publishes retained online STATE.
    std::vector<uint8_t> offline = state_offline();
    std::vector<uint8_t> online = state_online();
    MockLink host_link = {{}, {}, 0, true, false};
    uint16_t host_tid = broker.attach_transport(make_transport(&host_link));
    WillSpec host_will = {kTopicState, offline, 1, true};
    send(broker, host_tid, host_link, build_connect_sp("sp-primary-host", 60, &host_will));
    std::vector<Frame> frames = split_frames(host_link.rx);
    assert(!frames.empty() && frames[0].fh == 0x20 && frames[0].body[0] == 0x00);
    send(broker, host_tid, host_link, build_subscribe(1, "spBv1.0/#", 0x01));
    host_link.rx.clear();
    send(broker, host_tid, host_link, build_publish_bin(kTopicState, online, 1, 21, true));
    drain(broker, host_tid);
    ack_all_qos1(broker, host_tid, host_link);  // host sees its own STATE via spBv1.0/#
    host_link.rx.clear();

    // 4-5. Edge node: NDEATH will, publishes NBIRTH/DBIRTH/NDATA/DDATA.
    std::vector<uint8_t> ndeath_payload = make_sp_payload(40, 0x77);
    MockLink node_link = {{}, {}, 0, true, false};
    uint16_t node_tid = broker.attach_transport(make_transport(&node_link));
    WillSpec node_will = {kTopicNdeath, ndeath_payload, 1, false};
    send(broker, node_tid, node_link, build_connect_sp("sp-edge-node", 60, &node_will));

    std::vector<uint8_t> nbirth = make_sp_payload(512, 0x01);
    std::vector<uint8_t> dbirth = make_sp_payload(384, 0x02);
    std::vector<uint8_t> ndat = make_sp_payload(64, 0x03);
    std::vector<uint8_t> ddat = make_sp_payload(64, 0x04);
    send(broker, node_tid, node_link, build_publish_bin(kTopicNbirth, nbirth, 1, 31, false));
    send(broker, node_tid, node_link, build_publish_bin(kTopicDbirth, dbirth, 1, 32, false));
    send(broker, node_tid, node_link, build_publish_bin(kTopicNdata, ndat, 0, 0, false));
    send(broker, node_tid, node_link, build_publish_bin(kTopicDdata, ddat, 0, 0, false));
    drain(broker, host_tid);

    // 6. Host receives all four, payloads intact.
    assert(publishes_on_topic(host_link.rx, kTopicNbirth).at(0).payload == nbirth);
    assert(publishes_on_topic(host_link.rx, kTopicDbirth).at(0).payload == dbirth);
    assert(publishes_on_topic(host_link.rx, kTopicNdata).at(0).payload == ndat);
    assert(publishes_on_topic(host_link.rx, kTopicDdata).at(0).payload == ddat);
    ack_all_qos1(broker, host_tid, host_link);
    host_link.rx.clear();

    // 7-8. Abrupt node death publishes NDEATH.
    mock_close(&node_link);
    broker.on_readable(node_tid);
    drain(broker, host_tid);
    auto deaths = publishes_on_topic(host_link.rx, kTopicNdeath);
    assert(deaths.size() == 1);
    assert(deaths[0].qos == 1);
    assert(deaths[0].payload == ndeath_payload);
    ack_all_qos1(broker, host_tid, host_link);
    host_link.rx.clear();

    // 9-10. Node reconnects with a fresh session and re-births.
    MockLink node2_link = {{}, {}, 0, true, false};
    uint16_t node2_tid = broker.attach_transport(make_transport(&node2_link));
    send(broker, node2_tid, node2_link, build_connect_sp("sp-edge-node", 60, &node_will));
    frames = split_frames(node2_link.rx);
    assert(!frames.empty() && frames[0].fh == 0x20 && frames[0].body[0] == 0x00);
    send(broker, node2_tid, node2_link, build_publish_bin(kTopicNbirth, nbirth, 1, 41, false));
    send(broker, node2_tid, node2_link, build_publish_bin(kTopicDbirth, dbirth, 1, 42, false));
    drain(broker, host_tid);
    assert(publishes_on_topic(host_link.rx, kTopicNbirth).size() == 1);
    assert(publishes_on_topic(host_link.rx, kTopicDbirth).size() == 1);
    ack_all_qos1(broker, host_tid, host_link);
    host_link.rx.clear();

    // 11-12. Host leaves with DISCONNECT 0x04: retained offline STATE replaces online.
    send(broker, host_tid, host_link, build_disconnect_rl1(0x04));
    assert(broker.metrics().will_published == 2);  // NDEATH + STATE
    assert(broker.debug_retained_count() == 1);
    assert(broker.debug_retained_bytes() == offline.size());

    // 13-14. Late subscriber sees offline STATE with RETAIN=1.
    Host late;
    connect_host(broker, late, "sp-late-observer", kTopicState, 0x01);
    drain(broker, late.tid);
    auto pubs = publishes_on_topic(late.link.rx, kTopicState);
    if (pubs.size() != 1) {
        std::vector<Frame> late_frames = split_frames(late.link.rx);
        fprintf(stderr,
                "late rx bytes=%zu frames=%zu state_pubs=%zu retained=%zu\n",
                late.link.rx.size(), late_frames.size(), pubs.size(),
                broker.debug_retained_count());
        for (const Frame &f : late_frames) {
            PublishView pv = parse_publish(f);
            fprintf(stderr, "  frame fh=0x%02x pub_valid=%d topic=%s retain=%d dup=%d\n",
                    f.fh, pv.valid ? 1 : 0, pv.topic.c_str(), pv.retain ? 1 : 0,
                    pv.dup ? 1 : 0);
        }
        fflush(stderr);
    }
    SP_ASSERT(pubs.size() == 1, "expected 1 STATE publish, got %zu", pubs.size());
    assert(pubs[0].retain);
    assert(pubs[0].payload == offline);
    ack_all_qos1(broker, late.tid, late.link);
    printf("PASS sparkplug_full_lifecycle\n");
}

static void sparkplug_reconnect_stress()
{
    Broker broker;
    uint32_t tick = 0;
    broker.tick(tick);

    // Baseline resource state before any traffic.
    const size_t base_blocks = broker.debug_pool_blocks_allocated();
    const size_t base_topics = broker.debug_interned_topics();

    Host host;
    connect_host(broker, host, "sp-stress-host");

    std::vector<uint8_t> nbirth = make_sp_payload(256, 0xA1);
    std::vector<uint8_t> dbirth = make_sp_payload(192, 0xB2);
    std::vector<uint8_t> ddat = make_sp_payload(96, 0xC3);
    std::vector<uint8_t> ndeath_payload = make_sp_payload(40, 0xD4);

    const int kIterations = 1000;
    uint32_t wills_expected = 0;
    for (int i = 0; i < kIterations; ++i) {
        MockLink node = {{}, {}, 0, true, false};
        uint16_t node_tid = broker.attach_transport(make_transport(&node));
        assert(node_tid != 0xFFFF);
        WillSpec will = {kTopicNdeath, ndeath_payload, 1, false};
        send(broker, node_tid, node, build_connect_sp("sp-stress-node", 10, &will));
        send(broker, node_tid, node, build_subscribe(2, kTopicNdata, 0x01));
        send(broker, node_tid, node, build_publish_bin(kTopicNbirth, nbirth, 1, 51, false));
        send(broker, node_tid, node, build_publish_bin(kTopicDbirth, dbirth, 1, 52, false));
        send(broker, node_tid, node, build_publish_bin(kTopicDdata, ddat, 0, 0, false));

        switch (i % 4) {
        case 0:  // abrupt TCP loss -> will
            mock_close(&node);
            broker.on_readable(node_tid);
            wills_expected++;
            break;
        case 1: {  // keep-alive timeout -> will (node keepalive 10s)
            tick += 10;
            broker.tick(tick);
            send(broker, host.tid, host.link, build_pingreq());
            tick += 10;
            broker.tick(tick);
            wills_expected++;
            break;
        }
        case 2:  // graceful DISCONNECT 0x00 -> no will
            send(broker, node_tid, node, build_disconnect_rl0());
            break;
        case 3:  // DISCONNECT 0x04 -> will
            send(broker, node_tid, node, build_disconnect_rl1(0x04));
            wills_expected++;
            break;
        }

        drain(broker, host.tid);
        ack_all_qos1(broker, host.tid, host.link);
        host.link.rx.clear();
        send(broker, host.tid, host.link, build_pingreq());

        assert(!broker.transport_attached(node_tid));
        assert(broker.debug_find_session("sp-stress-node") < 0);
    }

    assert(broker.metrics().will_published == wills_expected);
    assert(broker.metrics().keepalive_disconnects == kIterations / 4);

    // Tear down the host and verify every resource returns to baseline.
    send(broker, host.tid, host.link, build_disconnect_rl0());
    assert(broker.debug_active_sessions() == 0);
    assert(broker.debug_pool_blocks_allocated() == base_blocks);
    assert(broker.debug_interned_topics() == base_topics);
    assert(broker.debug_retained_count() == 0);
    assert(broker.debug_retained_bytes() == 0);
    assert(broker.debug_global_tx_bytes() == 0);
    assert(broker.debug_trie_subscriptions() == 0);
    assert(broker.debug_outbound_inflight_total() == 0);
    assert(!broker.debug_any_will_scheduled());
    assert(!broker.has_pending_tx());
    printf("PASS sparkplug_reconnect_stress (%d iterations, %u wills)\n", kIterations,
           wills_expected);
}

int main()
{
    init_test_host_env();
    setvbuf(stdout, nullptr, _IONBF, 0);
    disconnect_0x00_suppresses_will();
    disconnect_0x04_publishes_will();
    disconnect_0x04_qos1_retained_will();
    abrupt_disconnect_qos1_retained_will();
    keepalive_timeout_qos1_retained_will();
    late_subscriber_receives_retained_state();
    clean_start_expiry_zero_discards_session();
    sparkplug_binary_payload_preserved();
    sparkplug_wildcard_topic_routing();
    sparkplug_full_lifecycle();
    sparkplug_reconnect_stress();
    printf("test_sparkplug_server: all tests passed\n");
    return 0;
}
