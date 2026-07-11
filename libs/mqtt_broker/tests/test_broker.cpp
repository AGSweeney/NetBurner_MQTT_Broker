// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#include "mqtt_broker/broker.hpp"
#include "mqtt_broker/broker_policy.hpp"
#include "mqtt_broker/encode.hpp"
#include "mqtt_broker/mqtt_types.hpp"
#include "mqtt_broker/varint.hpp"
#include "mqtt_broker/wire.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace mqtt_broker;

struct MockLink {
    std::vector<uint8_t> rx;
    std::vector<uint8_t> tx;
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

static int mock_read_one(void *ctx, uint8_t *buf, size_t cap)
{
    auto *link = static_cast<MockLink *>(ctx);
    if (!link->open) {
        return 0;
    }
    if (link->read_off >= link->tx.size() || cap == 0) {
        return -1;
    }
    buf[0] = link->tx[link->read_off++];
    return 1;
}

static int mock_write_none(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    (void)data;
    (void)len;
    return 0;
}

static std::vector<uint8_t> build_connect(const char *client_id, uint16_t keep_alive = 60)
{
    std::vector<uint8_t> body;
    const char proto[] = {'M', 'Q', 'T', 'T'};
    body.push_back(0x00);
    body.push_back(0x04);
    body.insert(body.end(), proto, proto + 4);
    body.push_back(0x05);
    body.push_back(0x02);
    body.push_back(static_cast<uint8_t>((keep_alive >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(keep_alive & 0xFFu));
    body.push_back(0x00);

    size_t cid_len = std::strlen(client_id);
    body.push_back(static_cast<uint8_t>((cid_len >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(cid_len & 0xFFu));
    body.insert(body.end(), client_id, client_id + cid_len);

    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(body.size()), rl, sizeof(rl));
    std::vector<uint8_t> pkt;
    pkt.push_back(0x10);
    pkt.insert(pkt.end(), rl, rl + rl_size);
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

static std::vector<uint8_t> build_subscribe(uint16_t pid, const char *filter, uint8_t options)
{
    std::vector<uint8_t> body;
    body.push_back(static_cast<uint8_t>((pid >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(pid & 0xFFu));
    body.push_back(0x00);
    size_t flen = std::strlen(filter);
    body.push_back(static_cast<uint8_t>((flen >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(flen & 0xFFu));
    body.insert(body.end(), filter, filter + flen);
    body.push_back(options);

    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(body.size()), rl, sizeof(rl));
    std::vector<uint8_t> pkt;
    pkt.push_back(0x82);
    pkt.insert(pkt.end(), rl, rl + rl_size);
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

static std::vector<uint8_t> build_connect_auth(const char *client_id, const char *username,
                                               const char *password)
{
    std::vector<uint8_t> body;
    const char proto[] = {'M', 'Q', 'T', 'T'};
    body.push_back(0x00);
    body.push_back(0x04);
    body.insert(body.end(), proto, proto + 4);
    body.push_back(0x05);
    uint8_t flags = 0x02;  // clean start
    if (username != nullptr) {
        flags |= 0x80;
    }
    if (password != nullptr) {
        flags |= 0x40;
    }
    body.push_back(flags);
    body.push_back(0x00);
    body.push_back(0x3C);
    body.push_back(0x00);  // property length

    size_t cid_len = std::strlen(client_id);
    body.push_back(static_cast<uint8_t>((cid_len >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(cid_len & 0xFFu));
    body.insert(body.end(), client_id, client_id + cid_len);
    if (username != nullptr) {
        size_t ulen = std::strlen(username);
        body.push_back(static_cast<uint8_t>((ulen >> 8) & 0xFFu));
        body.push_back(static_cast<uint8_t>(ulen & 0xFFu));
        body.insert(body.end(), username, username + ulen);
    }
    if (password != nullptr) {
        size_t plen = std::strlen(password);
        body.push_back(static_cast<uint8_t>((plen >> 8) & 0xFFu));
        body.push_back(static_cast<uint8_t>(plen & 0xFFu));
        body.insert(body.end(), password, password + plen);
    }

    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(body.size()), rl, sizeof(rl));
    std::vector<uint8_t> pkt;
    pkt.push_back(0x10);
    pkt.insert(pkt.end(), rl, rl + rl_size);
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

static std::vector<uint8_t> build_connect_will(const char *client_id, const char *will_topic,
                                               const char *will_payload, uint8_t will_qos,
                                               bool will_retain)
{
    std::vector<uint8_t> body;
    const char proto[] = {'M', 'Q', 'T', 'T'};
    body.push_back(0x00);
    body.push_back(0x04);
    body.insert(body.end(), proto, proto + 4);
    body.push_back(0x05);
    uint8_t flags = 0x02 | 0x04;  // clean start + will flag
    flags |= static_cast<uint8_t>((will_qos & 0x03u) << 3);
    if (will_retain) {
        flags |= 0x20;
    }
    body.push_back(flags);
    body.push_back(0x00);
    body.push_back(0x3C);
    body.push_back(0x00);  // CONNECT property length

    size_t cid_len = std::strlen(client_id);
    body.push_back(static_cast<uint8_t>((cid_len >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(cid_len & 0xFFu));
    body.insert(body.end(), client_id, client_id + cid_len);

    body.push_back(0x00);  // will property length
    size_t wtlen = std::strlen(will_topic);
    body.push_back(static_cast<uint8_t>((wtlen >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(wtlen & 0xFFu));
    body.insert(body.end(), will_topic, will_topic + wtlen);
    size_t wplen = std::strlen(will_payload);
    body.push_back(static_cast<uint8_t>((wplen >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(wplen & 0xFFu));
    body.insert(body.end(), will_payload, will_payload + wplen);

    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(body.size()), rl, sizeof(rl));
    std::vector<uint8_t> pkt;
    pkt.push_back(0x10);
    pkt.insert(pkt.end(), rl, rl + rl_size);
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

static std::vector<uint8_t> build_disconnect(uint8_t reason, bool include_reason = true)
{
    if (!include_reason) {
        return {0xE0, 0x00};
    }
    return {0xE0, 0x02, reason, 0x00};
}

static std::vector<uint8_t> build_unsubscribe(uint16_t pid, const char *filter)
{
    std::vector<uint8_t> body;
    body.push_back(static_cast<uint8_t>((pid >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(pid & 0xFFu));
    body.push_back(0x00);  // property length
    size_t flen = std::strlen(filter);
    body.push_back(static_cast<uint8_t>((flen >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(flen & 0xFFu));
    body.insert(body.end(), filter, filter + flen);

    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(body.size()), rl, sizeof(rl));
    std::vector<uint8_t> pkt;
    pkt.push_back(0xA2);
    pkt.insert(pkt.end(), rl, rl + rl_size);
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

static std::vector<uint8_t> build_publish(const char *topic, const char *payload, bool retain = false)
{
    std::vector<uint8_t> body;
    size_t tlen = std::strlen(topic);
    body.push_back(static_cast<uint8_t>((tlen >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(tlen & 0xFFu));
    body.insert(body.end(), topic, topic + tlen);
    body.push_back(0x00);
    size_t plen = std::strlen(payload);
    body.insert(body.end(), payload, payload + plen);

    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(body.size()), rl, sizeof(rl));
    std::vector<uint8_t> pkt;
    pkt.push_back(retain ? static_cast<uint8_t>(0x31) : static_cast<uint8_t>(0x30));
    pkt.insert(pkt.end(), rl, rl + rl_size);
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

static std::vector<uint8_t> build_connect_persistent(const char *client_id, uint32_t expiry)
{
    std::vector<uint8_t> body;
    const char proto[] = {'M', 'Q', 'T', 'T'};
    body.push_back(0x00);
    body.push_back(0x04);
    body.insert(body.end(), proto, proto + 4);
    body.push_back(0x05);
    body.push_back(0x00);  // clean start = 0
    body.push_back(0x00);
    body.push_back(0x3c);
    body.push_back(0x05);  // property length
    body.push_back(SessionExpiryInterval);
    body.push_back(static_cast<uint8_t>((expiry >> 24) & 0xFFu));
    body.push_back(static_cast<uint8_t>((expiry >> 16) & 0xFFu));
    body.push_back(static_cast<uint8_t>((expiry >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(expiry & 0xFFu));

    size_t cid_len = std::strlen(client_id);
    body.push_back(static_cast<uint8_t>((cid_len >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(cid_len & 0xFFu));
    body.insert(body.end(), client_id, client_id + cid_len);

    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(body.size()), rl, sizeof(rl));
    std::vector<uint8_t> pkt;
    pkt.push_back(0x10);
    pkt.insert(pkt.end(), rl, rl + rl_size);
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

static std::vector<uint8_t> build_publish_ex(const char *topic, const char *payload, uint8_t qos,
                                             uint16_t pid, const std::vector<uint8_t> &props,
                                             bool retain = false)
{
    std::vector<uint8_t> body;
    size_t tlen = std::strlen(topic);
    body.push_back(static_cast<uint8_t>((tlen >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(tlen & 0xFFu));
    body.insert(body.end(), topic, topic + tlen);
    if (qos > 0) {
        body.push_back(static_cast<uint8_t>((pid >> 8) & 0xFFu));
        body.push_back(static_cast<uint8_t>(pid & 0xFFu));
    }
    assert(props.size() < 128);
    body.push_back(static_cast<uint8_t>(props.size()));
    body.insert(body.end(), props.begin(), props.end());
    size_t plen = std::strlen(payload);
    body.insert(body.end(), payload, payload + plen);

    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(body.size()), rl, sizeof(rl));
    std::vector<uint8_t> pkt;
    uint8_t fh = 0x30 | static_cast<uint8_t>((qos & 0x03u) << 1) | (retain ? 0x01u : 0x00u);
    pkt.push_back(fh);
    pkt.insert(pkt.end(), rl, rl + rl_size);
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

static bool contains_seq(const std::vector<uint8_t> &hay, const std::vector<uint8_t> &needle)
{
    if (needle.empty() || hay.size() < needle.size()) {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        if (std::memcmp(hay.data() + i, needle.data(), needle.size()) == 0) {
            return true;
        }
    }
    return false;
}

static std::vector<uint8_t> build_connect_limited(const char *client_id, uint32_t max_packet)
{
    std::vector<uint8_t> body;
    const char proto[] = {'M', 'Q', 'T', 'T'};
    body.push_back(0x00);
    body.push_back(0x04);
    body.insert(body.end(), proto, proto + 4);
    body.push_back(0x05);
    body.push_back(0x02);
    body.push_back(0x00);
    body.push_back(0x3c);
    body.push_back(0x05);
    body.push_back(MaximumPacketSize);
    body.push_back(static_cast<uint8_t>((max_packet >> 24) & 0xFFu));
    body.push_back(static_cast<uint8_t>((max_packet >> 16) & 0xFFu));
    body.push_back(static_cast<uint8_t>((max_packet >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(max_packet & 0xFFu));

    size_t cid_len = std::strlen(client_id);
    body.push_back(static_cast<uint8_t>((cid_len >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(cid_len & 0xFFu));
    body.insert(body.end(), client_id, client_id + cid_len);

    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(body.size()), rl, sizeof(rl));
    std::vector<uint8_t> pkt;
    pkt.push_back(0x10);
    pkt.insert(pkt.end(), rl, rl + rl_size);
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

static void pump(Broker &broker, uint16_t tid, MockLink &link)
{
    broker.on_readable(tid);
    broker.drain_tx(tid);
}

static void test_connect_and_ping()
{
    Broker broker;
    MockLink pub = {{}, {}, 0, true, false};
    BrokerTransportOps ops = {mock_read, mock_write, mock_close};
    BrokerTransport t = {&pub, ops, true};
    uint16_t tid = broker.attach_transport(t);
    assert(tid != 0xFFFF);

    pub.tx = build_connect("publisher");
    pump(broker, tid, pub);
    assert(broker.connected_count() == 1);
    assert(!pub.rx.empty());
    assert(pub.rx[0] == 0x20);

    pub.tx.clear();
    pub.read_off = 0;
    pub.tx = {0xC0, 0x00};
    pump(broker, tid, pub);
    bool pingresp = false;
    for (size_t i = 0; i + 1 < pub.rx.size(); ++i) {
        if (pub.rx[i] == 0xD0 && pub.rx[i + 1] == 0x00) {
            pingresp = true;
            break;
        }
    }
    assert(pingresp);
}

static void test_routing_qos0()
{
    Broker broker;
    MockLink pub = {{}, {}, 0, true, false};
    MockLink sub = {{}, {}, 0, true, false};
    uint16_t pub_tid = broker.attach_transport({&pub, {mock_read, mock_write, mock_close}, true});
    uint16_t sub_tid = broker.attach_transport({&sub, {mock_read, mock_write, mock_close}, true});

    pub.tx = build_connect("publisher");
    pump(broker, pub_tid, pub);
    sub.tx = build_connect("subscriber");
    pump(broker, sub_tid, sub);

    sub.rx.clear();
    sub.tx.clear();
    sub.read_off = 0;
    sub.tx = build_subscribe(1, "sensors/#", 0x00);
    pump(broker, sub_tid, sub);

    pub.tx.clear();
    pub.read_off = 0;
    pub.tx = build_publish("sensors/a", "hello");
    pump(broker, pub_tid, pub);
    broker.drain_tx(sub_tid);

    assert(sub.rx.size() > 4);
    assert(broker.metrics().publish_received == 1);
    assert(broker.metrics().publish_sent >= 1);
}

static void client_maximum_packet_size_limits_outbound()
{
    Broker broker;
    MockLink pub = {{}, {}, 0, true, false};
    MockLink sub_small = {{}, {}, 0, true, false};
    MockLink sub_ok = {{}, {}, 0, true, false};
    uint16_t pub_tid = broker.attach_transport({&pub, {mock_read, mock_write, mock_close}, true});
    uint16_t small_tid =
        broker.attach_transport({&sub_small, {mock_read, mock_write, mock_close}, true});
    uint16_t ok_tid = broker.attach_transport({&sub_ok, {mock_read, mock_write, mock_close}, true});

    char big_payload[200];
    std::memset(big_payload, 'x', sizeof(big_payload));
    big_payload[199] = '\0';
    assert(estimate_publish_qos0_size("sensors/a", 199, false) > 32);

    pub.tx = build_connect("publisher");
    pump(broker, pub_tid, pub);
    sub_small.tx = build_connect_limited("small", 32);
    pump(broker, small_tid, sub_small);
    assert(broker.debug_client_max_packet(small_tid) == 32);
    sub_ok.tx = build_connect("big");
    pump(broker, ok_tid, sub_ok);

    sub_small.tx = build_subscribe(1, "sensors/#", 0x00);
    sub_small.read_off = 0;
    pump(broker, small_tid, sub_small);
    sub_ok.tx = build_subscribe(1, "sensors/#", 0x00);
    sub_ok.read_off = 0;
    pump(broker, ok_tid, sub_ok);

    pub.tx = build_publish("sensors/a", "probe");
    pub.read_off = 0;
    pump(broker, pub_tid, pub);
    broker.drain_tx(small_tid);
    broker.drain_tx(ok_tid);
    size_t small_rx_after_probe = sub_small.rx.size();
    size_t ok_rx_after_probe = sub_ok.rx.size();
    pub.tx = build_publish("sensors/a", big_payload);
    pub.read_off = 0;
    pump(broker, pub_tid, pub);
    broker.drain_tx(small_tid);
    broker.drain_tx(ok_tid);

    assert(broker.metrics().dropped_too_large == 1);
    assert(sub_small.rx.size() == small_rx_after_probe);
    assert(sub_ok.rx.size() > ok_rx_after_probe);
    printf("PASS client_maximum_packet_size_limits_outbound\n");
}

static void tls_limit_matches_documented_profile()
{
    static_assert(BrokerLimits::MaxTcpClients == 32,
                  "SOMRT1061/host reference profile uses 32 broker session slots");
    static_assert(BrokerLimits::MaxTlsClients == 2,
                  "platform TLS accept cap is 2 simultaneous MQTTS clients");
    static_assert(BrokerLimits::MaxTlsClients <= BrokerLimits::MaxTcpClients,
                  "TLS clients consume broker session slots from the shared pool");
    printf("PASS tls_limit_matches_documented_profile\n");
}

static void test_too_large_per_subscriber()
{
    client_maximum_packet_size_limits_outbound();
}

static void test_slow_consumer_disconnect()
{
    Broker broker;
    MockLink pub = {{}, {}, 0, true, false};
    MockLink sub = {{}, {}, 0, true, false};
    uint16_t pub_tid = broker.attach_transport({&pub, {mock_read, mock_write, mock_close}, true});
    uint16_t sub_tid = broker.attach_transport({&sub, {mock_read, mock_write, mock_close}, true});

    pub.tx = build_connect("publisher");
    pump(broker, pub_tid, pub);
    sub.tx = build_connect("slow");
    pump(broker, sub_tid, sub);

    sub.tx = build_subscribe(1, "#", 0x00);
    sub.read_off = 0;
    pump(broker, sub_tid, sub);

    sub.block_writes = true;
    for (int i = 0; i < 70 && sub.open; ++i) {
        char topic[32];
        std::snprintf(topic, sizeof(topic), "flood/%d", i);
        pub.tx = build_publish(topic, "x");
        pub.read_off = 0;
        broker.on_readable(pub_tid);
        broker.drain_tx(sub_tid);
    }

    assert(!sub.open);
    assert(broker.metrics().slow_consumer_disconnects >= 1);
    assert(pub.open);
}

static void test_retained_on_subscribe()
{
    Broker broker;
    MockLink pub = {{}, {}, 0, true, false};
    MockLink sub = {{}, {}, 0, true, false};
    uint16_t pub_tid = broker.attach_transport({&pub, {mock_read, mock_write, mock_close}, true});
    uint16_t sub_tid = broker.attach_transport({&sub, {mock_read, mock_write, mock_close}, true});

    pub.tx = build_connect("publisher");
    pump(broker, pub_tid, pub);
    pub.tx = build_publish("home/status", "retained", true);
    pub.read_off = 0;
    pump(broker, pub_tid, pub);

    sub.tx = build_connect("subscriber");
    pump(broker, sub_tid, sub);
    sub.rx.clear();
    sub.tx = build_subscribe(1, "home/#", 0x00);
    sub.read_off = 0;
    pump(broker, sub_tid, sub);

    assert(sub.rx.size() > 8);
}

static void test_unsubscribe()
{
    Broker broker;
    MockLink pub = {{}, {}, 0, true, false};
    MockLink sub = {{}, {}, 0, true, false};
    uint16_t pub_tid = broker.attach_transport({&pub, {mock_read, mock_write, mock_close}, true});
    uint16_t sub_tid = broker.attach_transport({&sub, {mock_read, mock_write, mock_close}, true});

    pub.tx = build_connect("publisher");
    pump(broker, pub_tid, pub);
    sub.tx = build_connect("unsubber");
    pump(broker, sub_tid, sub);

    sub.tx = build_subscribe(1, "news/#", 0x00);
    sub.read_off = 0;
    pump(broker, sub_tid, sub);

    pub.tx = build_publish("news/a", "one");
    pub.read_off = 0;
    pump(broker, pub_tid, pub);
    broker.drain_tx(sub_tid);
    size_t rx_after_first = sub.rx.size();
    assert(rx_after_first > 0);

    sub.rx.clear();
    sub.tx = build_unsubscribe(2, "news/#");
    sub.read_off = 0;
    pump(broker, sub_tid, sub);

    // Expect UNSUBACK: 0xB0, rl, pid=2, prop len 0, reason 0x00
    bool unsuback = false;
    for (size_t i = 0; i + 5 < sub.rx.size() + 1 && i < sub.rx.size(); ++i) {
        if (sub.rx[i] == 0xB0) {
            assert(i + 6 <= sub.rx.size());
            assert(sub.rx[i + 2] == 0x00 && sub.rx[i + 3] == 0x02);
            assert(sub.rx[i + 5] == 0x00);
            unsuback = true;
            break;
        }
    }
    assert(unsuback);

    // No further delivery after unsubscribe.
    sub.rx.clear();
    pub.tx = build_publish("news/a", "two");
    pub.read_off = 0;
    pump(broker, pub_tid, pub);
    broker.drain_tx(sub_tid);
    assert(sub.rx.empty());

    // Unsubscribing a non-existent filter returns 0x11.
    sub.rx.clear();
    sub.tx = build_unsubscribe(3, "news/#");
    sub.read_off = 0;
    pump(broker, sub_tid, sub);
    bool no_sub_existed = false;
    for (size_t i = 0; i < sub.rx.size(); ++i) {
        if (sub.rx[i] == 0xB0 && i + 6 <= sub.rx.size()) {
            assert(sub.rx[i + 5] == 0x11);
            no_sub_existed = true;
            break;
        }
    }
    assert(no_sub_existed);
}

static void test_property_forwarding()
{
    Broker broker;
    MockLink pub = {{}, {}, 0, true, false};
    MockLink sub = {{}, {}, 0, true, false};
    uint16_t pub_tid = broker.attach_transport({&pub, {mock_read, mock_write, mock_close}, true});
    uint16_t sub_tid = broker.attach_transport({&sub, {mock_read, mock_write, mock_close}, true});

    pub.tx = build_connect("publisher");
    pump(broker, pub_tid, pub);
    sub.tx = build_connect("propsub");
    pump(broker, sub_tid, sub);

    sub.tx = build_subscribe(1, "props/#", 0x00);
    sub.read_off = 0;
    pump(broker, sub_tid, sub);
    sub.rx.clear();

    // PFI=1, ContentType "text/plain", ResponseTopic "r/t", UserProperty k=v
    std::vector<uint8_t> props = {0x01, 0x01};
    const char *ct = "text/plain";
    props.push_back(0x03);
    props.push_back(0x00);
    props.push_back(static_cast<uint8_t>(std::strlen(ct)));
    props.insert(props.end(), ct, ct + std::strlen(ct));
    props.push_back(0x08);
    props.push_back(0x00);
    props.push_back(0x03);
    props.insert(props.end(), {'r', '/', 't'});
    props.push_back(0x26);
    props.insert(props.end(), {0x00, 0x01, 'k', 0x00, 0x01, 'v'});

    pub.tx = build_publish_ex("props/a", "hello", 0, 0, props);
    pub.read_off = 0;
    pump(broker, pub_tid, pub);
    broker.drain_tx(sub_tid);

    assert(!sub.rx.empty());
    std::vector<uint8_t> ct_seq = {0x03, 0x00, 0x0A};
    ct_seq.insert(ct_seq.end(), ct, ct + std::strlen(ct));
    assert(contains_seq(sub.rx, ct_seq));
    std::vector<uint8_t> up_seq = {0x26, 0x00, 0x01, 'k', 0x00, 0x01, 'v'};
    assert(contains_seq(sub.rx, up_seq));
    std::vector<uint8_t> rt_seq = {0x08, 0x00, 0x03, 'r', '/', 't'};
    assert(contains_seq(sub.rx, rt_seq));
}

static void test_message_expiry()
{
    Broker broker;
    broker.tick(0);
    MockLink pub = {{}, {}, 0, true, false};
    uint16_t pub_tid = broker.attach_transport({&pub, {mock_read, mock_write, mock_close}, true});
    pub.tx = build_connect("publisher");
    pump(broker, pub_tid, pub);

    // Retained publish with Message Expiry Interval = 5 seconds.
    std::vector<uint8_t> props = {0x02, 0x00, 0x00, 0x00, 0x05};
    pub.tx = build_publish_ex("exp/t", "payload", 0, 0, props, true);
    pub.read_off = 0;
    pump(broker, pub_tid, pub);

    // Subscribe 2 seconds later: retained delivered with remaining = 3.
    broker.tick(2);
    MockLink sub1 = {{}, {}, 0, true, false};
    uint16_t sub1_tid =
        broker.attach_transport({&sub1, {mock_read, mock_write, mock_close}, true});
    sub1.tx = build_connect("expsub1");
    pump(broker, sub1_tid, sub1);
    sub1.rx.clear();
    sub1.tx = build_subscribe(1, "exp/#", 0x00);
    sub1.read_off = 0;
    pump(broker, sub1_tid, sub1);
    std::vector<uint8_t> remaining3 = {0x02, 0x00, 0x00, 0x00, 0x03};
    assert(contains_seq(sub1.rx, remaining3));

    // After expiry the retained message is swept and no longer delivered.
    broker.tick(10);
    MockLink sub2 = {{}, {}, 0, true, false};
    uint16_t sub2_tid =
        broker.attach_transport({&sub2, {mock_read, mock_write, mock_close}, true});
    sub2.tx = build_connect("expsub2");
    pump(broker, sub2_tid, sub2);
    sub2.rx.clear();
    sub2.tx = build_subscribe(1, "exp/#", 0x00);
    sub2.read_off = 0;
    pump(broker, sub2_tid, sub2);
    bool got_publish = false;
    for (size_t i = 0; i < sub2.rx.size(); ++i) {
        if ((sub2.rx[i] & 0xF0u) == 0x30u) {
            got_publish = true;
        }
    }
    assert(!got_publish);
    assert(broker.metrics().messages_expired >= 1);
}

static void test_qos1_retransmit_on_resume()
{
    Broker broker;
    broker.tick(0);
    MockLink pub = {{}, {}, 0, true, false};
    MockLink sub = {{}, {}, 0, true, false};
    uint16_t pub_tid = broker.attach_transport({&pub, {mock_read, mock_write, mock_close}, true});
    uint16_t sub_tid = broker.attach_transport({&sub, {mock_read, mock_write, mock_close}, true});

    pub.tx = build_connect("publisher");
    pump(broker, pub_tid, pub);
    sub.tx = build_connect_persistent("resumer", 100);
    pump(broker, sub_tid, sub);

    sub.tx = build_subscribe(1, "rt/#", 0x01);
    sub.read_off = 0;
    pump(broker, sub_tid, sub);
    sub.rx.clear();

    pub.tx = build_publish_ex("rt/a", "msg", 1, 9, {});
    pub.read_off = 0;
    pump(broker, pub_tid, pub);
    broker.drain_tx(sub_tid);

    // PUBLISH QoS1 received; extract its packet id (fh 0x32).
    assert(!sub.rx.empty());
    size_t idx = 0;
    while (idx < sub.rx.size() && sub.rx[idx] != 0x32) {
        idx++;
    }
    assert(idx + 4 < sub.rx.size());
    size_t tlen = (static_cast<size_t>(sub.rx[idx + 2]) << 8) | sub.rx[idx + 3];
    uint16_t sent_pid = static_cast<uint16_t>((sub.rx[idx + 4 + tlen] << 8) |
                                              sub.rx[idx + 4 + tlen + 1]);
    assert(sent_pid != 0);

    // Abrupt disconnect without PUBACK; session stays alive (expiry 100).
    mock_close(&sub);
    broker.on_readable(sub_tid);
    assert(!broker.transport_attached(sub_tid));

    // Resume: expect session-present CONNACK and a DUP retransmission (fh 0x3A).
    MockLink sub2 = {{}, {}, 0, true, false};
    uint16_t sub2_tid =
        broker.attach_transport({&sub2, {mock_read, mock_write, mock_close}, true});
    sub2.tx = build_connect_persistent("resumer", 100);
    pump(broker, sub2_tid, sub2);
    broker.drain_tx(sub2_tid);

    assert(sub2.rx.size() > 4);
    assert(sub2.rx[0] == 0x20);
    assert(sub2.rx[2] == 0x01);  // session present

    size_t dup_idx = 0;
    bool found_dup = false;
    for (size_t i = 0; i < sub2.rx.size(); ++i) {
        if (sub2.rx[i] == 0x3A) {
            dup_idx = i;
            found_dup = true;
            break;
        }
    }
    assert(found_dup);
    size_t tlen2 = (static_cast<size_t>(sub2.rx[dup_idx + 2]) << 8) | sub2.rx[dup_idx + 3];
    uint16_t resent_pid = static_cast<uint16_t>((sub2.rx[dup_idx + 4 + tlen2] << 8) |
                                                sub2.rx[dup_idx + 4 + tlen2 + 1]);
    assert(resent_pid == sent_pid);

    // PUBACK now releases the inflight entry; a further reconnect resends nothing.
    std::vector<uint8_t> puback = {0x40, 0x02,
                                   static_cast<uint8_t>((sent_pid >> 8) & 0xFFu),
                                   static_cast<uint8_t>(sent_pid & 0xFFu)};
    sub2.tx = puback;
    sub2.read_off = 0;
    pump(broker, sub2_tid, sub2);

    mock_close(&sub2);
    broker.on_readable(sub2_tid);
    MockLink sub3 = {{}, {}, 0, true, false};
    uint16_t sub3_tid =
        broker.attach_transport({&sub3, {mock_read, mock_write, mock_close}, true});
    sub3.tx = build_connect_persistent("resumer", 100);
    pump(broker, sub3_tid, sub3);
    broker.drain_tx(sub3_tid);
    for (size_t i = 0; i < sub3.rx.size(); ++i) {
        assert(sub3.rx[i] != 0x3A);
    }
}

// Finds the first PUBLISH in rx and returns its fixed-header byte, or 0.
static uint8_t first_publish_header(const std::vector<uint8_t> &rx)
{
    for (size_t i = 0; i < rx.size(); ++i) {
        if ((rx[i] & 0xF0u) == 0x30u) {
            return rx[i];
        }
    }
    return 0;
}

static void test_disconnect_with_will()
{
    Broker broker;
    MockLink sub = {{}, {}, 0, true, false};
    uint16_t sub_tid = broker.attach_transport({&sub, {mock_read, mock_write, mock_close}, true});
    sub.tx = build_connect("will-watcher");
    pump(broker, sub_tid, sub);
    sub.tx = build_subscribe(1, "state/#", 0x01);  // request QoS 1
    sub.read_off = 0;
    pump(broker, sub_tid, sub);
    sub.rx.clear();

    // Client with retained QoS 1 will sends DISCONNECT 0x04: will must publish.
    MockLink dev = {{}, {}, 0, true, false};
    uint16_t dev_tid = broker.attach_transport({&dev, {mock_read, mock_write, mock_close}, true});
    dev.tx = build_connect_will("dev-0x04", "state/dev1", "offline", 1, true);
    pump(broker, dev_tid, dev);
    dev.tx = build_disconnect(0x04);
    dev.read_off = 0;
    pump(broker, dev_tid, dev);
    broker.drain_tx(sub_tid);

    uint8_t fh = first_publish_header(sub.rx);
    assert(fh != 0);
    assert(((fh >> 1) & 0x03u) == 1);  // delivered at QoS 1
    assert(broker.metrics().will_published == 1);

    // Late subscriber gets the will as a retained QoS 1 message.
    MockLink late = {{}, {}, 0, true, false};
    uint16_t late_tid = broker.attach_transport({&late, {mock_read, mock_write, mock_close}, true});
    late.tx = build_connect("late-sub");
    pump(broker, late_tid, late);
    late.rx.clear();
    late.tx = build_subscribe(1, "state/dev1", 0x01);
    late.read_off = 0;
    pump(broker, late_tid, late);
    broker.drain_tx(late_tid);

    fh = first_publish_header(late.rx);
    assert(fh != 0);
    assert((fh & 0x01u) != 0);         // RETAIN set
    assert(((fh >> 1) & 0x03u) == 1);  // QoS 1

    // Normal DISCONNECT (0x00) suppresses the will.
    sub.rx.clear();
    MockLink dev2 = {{}, {}, 0, true, false};
    uint16_t dev2_tid = broker.attach_transport({&dev2, {mock_read, mock_write, mock_close}, true});
    dev2.tx = build_connect_will("dev-normal", "state/dev2", "offline", 1, false);
    pump(broker, dev2_tid, dev2);
    dev2.tx = build_disconnect(0x00, false);  // remaining length 0
    dev2.read_off = 0;
    pump(broker, dev2_tid, dev2);
    broker.drain_tx(sub_tid);
    assert(first_publish_header(sub.rx) == 0);
    assert(broker.metrics().will_published == 1);

    // DISCONNECT 0x00 with explicit reason byte also suppresses.
    sub.rx.clear();
    MockLink dev3 = {{}, {}, 0, true, false};
    uint16_t dev3_tid = broker.attach_transport({&dev3, {mock_read, mock_write, mock_close}, true});
    dev3.tx = build_connect_will("dev-normal2", "state/dev3", "offline", 1, false);
    pump(broker, dev3_tid, dev3);
    dev3.tx = build_disconnect(0x00);
    dev3.read_off = 0;
    pump(broker, dev3_tid, dev3);
    broker.drain_tx(sub_tid);
    assert(first_publish_header(sub.rx) == 0);
    assert(broker.metrics().will_published == 1);
}

static ReasonCode test_auth_check(const char *client_id, const char *username,
                                  const uint8_t *password, size_t password_len)
{
    (void)client_id;
    if (username == nullptr) {
        return ReasonCode::NotAuthorized;
    }
    if (std::strcmp(username, "alice") == 0 && password != nullptr && password_len == 6 &&
        std::memcmp(password, "secret", 6) == 0) {
        return ReasonCode::Success;
    }
    return ReasonCode::BadUserNameOrPassword;
}

static void test_connect_authentication()
{
    broker_set_auth_handler(test_auth_check);
    Broker broker;

    // Correct credentials accepted.
    MockLink good = {{}, {}, 0, true, false};
    uint16_t good_tid = broker.attach_transport({&good, {mock_read, mock_write, mock_close}, true});
    good.tx = build_connect_auth("authok", "alice", "secret");
    pump(broker, good_tid, good);
    assert(good.rx.size() >= 4);
    assert(good.rx[0] == 0x20);
    assert(good.rx[3] == 0x00);
    assert(broker.connected_count() == 1);

    // Wrong password: CONNACK 0x86 and the connection is closed.
    MockLink bad = {{}, {}, 0, true, false};
    uint16_t bad_tid = broker.attach_transport({&bad, {mock_read, mock_write, mock_close}, true});
    bad.tx = build_connect_auth("authbad", "alice", "wrong");
    pump(broker, bad_tid, bad);
    assert(bad.rx.size() >= 4);
    assert(bad.rx[0] == 0x20);
    assert(bad.rx[3] == 0x86);
    assert(!bad.open);

    // No credentials: CONNACK 0x87.
    MockLink anon = {{}, {}, 0, true, false};
    uint16_t anon_tid = broker.attach_transport({&anon, {mock_read, mock_write, mock_close}, true});
    anon.tx = build_connect_auth("authanon", nullptr, nullptr);
    pump(broker, anon_tid, anon);
    assert(anon.rx.size() >= 4);
    assert(anon.rx[0] == 0x20);
    assert(anon.rx[3] == 0x87);
    assert(!anon.open);

    assert(broker.metrics().connect_reject == 2);
    assert(broker.connected_count() == 1);

    broker_set_auth_handler(nullptr);
}

static void test_fragmented_connect()
{
    Broker broker;
    MockLink link = {{}, {}, 0, true, false};
    uint16_t tid =
        broker.attach_transport({&link, {mock_read_one, mock_write, mock_close}, true});
    link.tx = build_connect("frag_client");
    link.read_off = 0;

    while (link.read_off < link.tx.size()) {
        broker.on_readable(tid);
    }
    assert(broker.connected_count() == 1);
}

static void test_transport_slot_released_on_eof()
{
    Broker broker;
    MockLink link = {{}, {}, 0, true, false};
    uint16_t tid = broker.attach_transport({&link, {mock_read, mock_write, mock_close}, true});
    link.tx = build_connect("slot_release");
    pump(broker, tid, link);
    assert(broker.transport_attached(tid));

    mock_close(&link);
    broker.on_readable(tid);
    assert(!broker.transport_attached(tid));

    MockLink link2 = {{}, {}, 0, true, false};
    uint16_t tid2 = broker.attach_transport({&link2, {mock_read, mock_write, mock_close}, true});
    assert(tid2 != 0xFFFF);
    link2.tx = build_connect("slot_release_2");
    pump(broker, tid2, link2);
    assert(broker.connected_count() == 1);
}

// ---- MQTT 3.1.1 (protocol level 4) interop ----

static std::vector<uint8_t> build_connect_v4(const char *client_id, uint16_t keep_alive = 60,
                                             bool clean_session = true)
{
    std::vector<uint8_t> body;
    const char proto[] = {'M', 'Q', 'T', 'T'};
    body.push_back(0x00);
    body.push_back(0x04);
    body.insert(body.end(), proto, proto + 4);
    body.push_back(0x04);  // protocol level 4 = MQTT 3.1.1
    body.push_back(clean_session ? 0x02 : 0x00);
    body.push_back(static_cast<uint8_t>((keep_alive >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(keep_alive & 0xFFu));
    // No properties section in 3.1.1 — client id payload follows directly.
    size_t cid_len = std::strlen(client_id);
    body.push_back(static_cast<uint8_t>((cid_len >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(cid_len & 0xFFu));
    body.insert(body.end(), client_id, client_id + cid_len);

    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(body.size()), rl, sizeof(rl));
    std::vector<uint8_t> pkt;
    pkt.push_back(0x10);
    pkt.insert(pkt.end(), rl, rl + rl_size);
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

static std::vector<uint8_t> build_subscribe_v4(uint16_t pid, const char *filter, uint8_t qos)
{
    std::vector<uint8_t> body;
    body.push_back(static_cast<uint8_t>((pid >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(pid & 0xFFu));
    // No property length byte in 3.1.1.
    size_t flen = std::strlen(filter);
    body.push_back(static_cast<uint8_t>((flen >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(flen & 0xFFu));
    body.insert(body.end(), filter, filter + flen);
    body.push_back(qos);

    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(body.size()), rl, sizeof(rl));
    std::vector<uint8_t> pkt;
    pkt.push_back(0x82);
    pkt.insert(pkt.end(), rl, rl + rl_size);
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

static std::vector<uint8_t> build_publish_v4(const char *topic, const char *payload,
                                             uint8_t qos = 0, uint16_t pid = 0)
{
    std::vector<uint8_t> body;
    size_t tlen = std::strlen(topic);
    body.push_back(static_cast<uint8_t>((tlen >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(tlen & 0xFFu));
    body.insert(body.end(), topic, topic + tlen);
    if (qos > 0) {
        body.push_back(static_cast<uint8_t>((pid >> 8) & 0xFFu));
        body.push_back(static_cast<uint8_t>(pid & 0xFFu));
    }
    // No property length byte in 3.1.1.
    size_t plen = std::strlen(payload);
    body.insert(body.end(), payload, payload + plen);

    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(body.size()), rl, sizeof(rl));
    std::vector<uint8_t> pkt;
    pkt.push_back(0x30 | static_cast<uint8_t>((qos & 0x03u) << 1));
    pkt.insert(pkt.end(), rl, rl + rl_size);
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

static void test_v4_connect_and_connack_format()
{
    Broker broker;
    MockLink link = {{}, {}, 0, true, false};
    uint16_t tid = broker.attach_transport({&link, {mock_read, mock_write, mock_close}, true});

    link.tx = build_connect_v4("v4client");
    pump(broker, tid, link);
    assert(broker.connected_count() == 1);
    // MQTT 3.1.1 CONNACK: exactly 4 bytes — 0x20 0x02 <ack flags> <return code>.
    assert(link.rx.size() == 4);
    assert(link.rx[0] == 0x20 && link.rx[1] == 0x02 && link.rx[2] == 0x00 && link.rx[3] == 0x00);

    // PINGREQ/PINGRESP unchanged in 3.1.1.
    link.rx.clear();
    link.tx = {0xC0, 0x00};
    link.read_off = 0;
    pump(broker, tid, link);
    assert(link.rx.size() == 2 && link.rx[0] == 0xD0 && link.rx[1] == 0x00);
}

static void test_v4_pub_sub_routing()
{
    Broker broker;
    MockLink pub = {{}, {}, 0, true, false};
    MockLink sub = {{}, {}, 0, true, false};
    uint16_t pub_tid = broker.attach_transport({&pub, {mock_read, mock_write, mock_close}, true});
    uint16_t sub_tid = broker.attach_transport({&sub, {mock_read, mock_write, mock_close}, true});

    pub.tx = build_connect_v4("v4pub");
    pump(broker, pub_tid, pub);
    sub.tx = build_connect_v4("v4sub");
    pump(broker, sub_tid, sub);

    sub.rx.clear();
    sub.tx = build_subscribe_v4(1, "v4/topic", 0x00);
    sub.read_off = 0;
    pump(broker, sub_tid, sub);
    // 3.1.1 SUBACK: 0x90 0x03 <pid hi> <pid lo> <granted qos> — no property byte.
    assert(sub.rx.size() == 5);
    assert(sub.rx[0] == 0x90 && sub.rx[1] == 0x03);
    assert(sub.rx[2] == 0x00 && sub.rx[3] == 0x01 && sub.rx[4] == 0x00);

    sub.rx.clear();
    pub.tx = build_publish_v4("v4/topic", "hi311");
    pub.read_off = 0;
    pump(broker, pub_tid, pub);
    broker.drain_tx(sub_tid);

    // Delivered 3.1.1 PUBLISH: fh 0x30, RL = 2 + tlen + payload (no property byte).
    const char expected_topic[] = "v4/topic";
    size_t tlen = sizeof(expected_topic) - 1;
    assert(sub.rx.size() == 2 + 2 + tlen + 5);
    assert(sub.rx[0] == 0x30);
    assert(sub.rx[1] == 2 + tlen + 5);
    assert(std::memcmp(sub.rx.data() + 4, expected_topic, tlen) == 0);
    assert(std::memcmp(sub.rx.data() + 4 + tlen, "hi311", 5) == 0);
}

static void test_v4_qos1_puback_format()
{
    Broker broker;
    MockLink pub = {{}, {}, 0, true, false};
    uint16_t tid = broker.attach_transport({&pub, {mock_read, mock_write, mock_close}, true});
    pub.tx = build_connect_v4("v4qos1");
    pump(broker, tid, pub);

    pub.rx.clear();
    pub.tx = build_publish_v4("v4/q1", "x", 1, 0x1234);
    pub.read_off = 0;
    pump(broker, tid, pub);
    // 3.1.1 PUBACK: 0x40 0x02 <pid hi> <pid lo> — no reason code, no properties.
    assert(pub.rx.size() == 4);
    assert(pub.rx[0] == 0x40 && pub.rx[1] == 0x02);
    assert(pub.rx[2] == 0x12 && pub.rx[3] == 0x34);
}

static void test_cross_version_routing()
{
    Broker broker;
    MockLink v5pub = {{}, {}, 0, true, false};
    MockLink v4sub = {{}, {}, 0, true, false};
    MockLink v5sub = {{}, {}, 0, true, false};
    uint16_t v5pub_tid =
        broker.attach_transport({&v5pub, {mock_read, mock_write, mock_close}, true});
    uint16_t v4sub_tid =
        broker.attach_transport({&v4sub, {mock_read, mock_write, mock_close}, true});
    uint16_t v5sub_tid =
        broker.attach_transport({&v5sub, {mock_read, mock_write, mock_close}, true});

    v5pub.tx = build_connect("v5pub");
    pump(broker, v5pub_tid, v5pub);
    v4sub.tx = build_connect_v4("v4sub");
    pump(broker, v4sub_tid, v4sub);
    v5sub.tx = build_connect("v5sub");
    pump(broker, v5sub_tid, v5sub);

    v4sub.rx.clear();
    v4sub.tx = build_subscribe_v4(1, "mix/#", 0x00);
    v4sub.read_off = 0;
    pump(broker, v4sub_tid, v4sub);

    v5sub.rx.clear();
    v5sub.tx = build_subscribe(2, "mix/#", 0x00);
    v5sub.read_off = 0;
    pump(broker, v5sub_tid, v5sub);

    // MQTT 5 publisher with a user property → both subscribers get the payload;
    // the 3.1.1 subscriber's copy must have no property section.
    std::vector<uint8_t> props;
    props.push_back(UserProperty);
    const char key[] = "k";
    const char val[] = "v";
    props.push_back(0x00);
    props.push_back(1);
    props.insert(props.end(), key, key + 1);
    props.push_back(0x00);
    props.push_back(1);
    props.insert(props.end(), val, val + 1);

    v4sub.rx.clear();
    v5sub.rx.clear();
    v5pub.tx = build_publish_ex("mix/data", "zz", 0, 0, props);
    v5pub.read_off = 0;
    pump(broker, v5pub_tid, v5pub);
    broker.drain_tx(v4sub_tid);
    broker.drain_tx(v5sub_tid);

    const char topic[] = "mix/data";
    size_t tlen = sizeof(topic) - 1;
    // v4 copy: fh + rl + topic len/name + payload only.
    assert(v4sub.rx.size() == 2 + 2 + tlen + 2);
    assert(v4sub.rx[0] == 0x30);
    assert(std::memcmp(v4sub.rx.data() + 4 + tlen, "zz", 2) == 0);
    // v5 copy carries the forwarded user property, so it is strictly larger.
    assert(v5sub.rx.size() > v4sub.rx.size());
    assert(contains_seq(v5sub.rx, {static_cast<uint8_t>('k')}));

    // Reverse direction: 3.1.1 publisher → MQTT 5 subscriber still delivers.
    MockLink v4pub = {{}, {}, 0, true, false};
    uint16_t v4pub_tid =
        broker.attach_transport({&v4pub, {mock_read, mock_write, mock_close}, true});
    v4pub.tx = build_connect_v4("v4pub");
    pump(broker, v4pub_tid, v4pub);

    v5sub.rx.clear();
    v4sub.rx.clear();
    v4pub.tx = build_publish_v4("mix/back", "yo");
    v4pub.read_off = 0;
    pump(broker, v4pub_tid, v4pub);
    broker.drain_tx(v5sub_tid);
    broker.drain_tx(v4sub_tid);
    assert(contains_seq(v5sub.rx, {static_cast<uint8_t>('y'), static_cast<uint8_t>('o')}));
    assert(contains_seq(v4sub.rx, {static_cast<uint8_t>('y'), static_cast<uint8_t>('o')}));
}

static void test_v4_clean_session_semantics()
{
    Broker broker;
    MockLink link = {{}, {}, 0, true, false};
    uint16_t tid = broker.attach_transport({&link, {mock_read, mock_write, mock_close}, true});
    // CleanSession=0: session must survive disconnect (no expiry in 3.1.1).
    link.tx = build_connect_v4("v4persist", 60, false);
    pump(broker, tid, link);
    link.tx = build_subscribe_v4(1, "keep/#", 0x01);
    link.read_off = 0;
    pump(broker, tid, link);

    broker.detach_transport(tid);
    broker.tick(1000000);  // would expire any finite deadline
    assert(broker.debug_session_present("v4persist"));

    // Reconnect with CleanSession=0 → session present flag set in CONNACK.
    MockLink again = {{}, {}, 0, true, false};
    uint16_t tid2 = broker.attach_transport({&again, {mock_read, mock_write, mock_close}, true});
    again.tx = build_connect_v4("v4persist", 60, false);
    pump(broker, tid2, again);
    assert(again.rx.size() == 4);
    assert(again.rx[2] == 0x01);  // session present
    assert(again.rx[3] == 0x00);
}

static void test_keepalive_disconnect()
{
    Broker broker;
    MockLink link = {{}, {}, 0, true, false};
    uint16_t tid = broker.attach_transport({&link, {mock_read, mock_write, mock_close}, true});
    link.tx = build_connect("ka_client", 10);
    pump(broker, tid, link);
    assert(link.open);

    broker.tick(0);
    broker.tick(15);
    assert(link.open);
    broker.tick(16);
    assert(!link.open);
    assert(broker.metrics().keepalive_disconnects == 1);
}

int main()
{
    test_connect_and_ping();
    test_routing_qos0();
    client_maximum_packet_size_limits_outbound();
    tls_limit_matches_documented_profile();
    test_slow_consumer_disconnect();
    test_retained_on_subscribe();
    test_unsubscribe();
    test_property_forwarding();
    test_message_expiry();
    test_qos1_retransmit_on_resume();
    test_connect_authentication();
    test_disconnect_with_will();
    test_fragmented_connect();
    test_transport_slot_released_on_eof();
    test_keepalive_disconnect();
    test_v4_connect_and_connack_format();
    test_v4_pub_sub_routing();
    test_v4_qos1_puback_format();
    test_cross_version_routing();
    test_v4_clean_session_semantics();
    return 0;
}