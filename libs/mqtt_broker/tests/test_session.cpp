#include "mqtt_broker/broker.hpp"
#include "mqtt_broker/message_pool.hpp"
#include "mqtt_broker/mqtt_types.hpp"
#include "mqtt_broker/offline_queue.hpp"
#include "mqtt_broker/retained_store.hpp"
#include "mqtt_broker/topic_intern.hpp"
#include "mqtt_broker/varint.hpp"

#include <cassert>
#include <cstring>
#include <vector>

using namespace mqtt_broker;

struct MockLink {
    std::vector<uint8_t> rx;
    std::vector<uint8_t> tx;
    size_t read_off;
    bool open;
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
    link->rx.insert(link->rx.end(), data, data + len);
    return static_cast<int>(len);
}

static void mock_close(void *ctx)
{
    static_cast<MockLink *>(ctx)->open = false;
}

static std::vector<uint8_t> build_connect(const char *client_id, bool clean_start = true,
                                          uint32_t session_expiry = 0)
{
    std::vector<uint8_t> body;
    const char proto[] = {'M', 'Q', 'T', 'T'};
    body.push_back(0x00);
    body.push_back(0x04);
    body.insert(body.end(), proto, proto + 4);
    body.push_back(0x05);
    body.push_back(clean_start ? 0x02u : 0x00u);
    body.push_back(0x00);
    body.push_back(0x3c);
    if (session_expiry > 0) {
        body.push_back(0x05);
        body.push_back(SessionExpiryInterval);
        body.push_back(static_cast<uint8_t>((session_expiry >> 24) & 0xFFu));
        body.push_back(static_cast<uint8_t>((session_expiry >> 16) & 0xFFu));
        body.push_back(static_cast<uint8_t>((session_expiry >> 8) & 0xFFu));
        body.push_back(static_cast<uint8_t>(session_expiry & 0xFFu));
    } else {
        body.push_back(0x00);
    }
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

static void append_u32_prop(std::vector<uint8_t> &body, PropertyId id, uint32_t value)
{
    body.push_back(0x05);
    body.push_back(static_cast<uint8_t>(id));
    body.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
    body.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
    body.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(value & 0xFFu));
}

static std::vector<uint8_t> build_connect_with_will(const char *client_id, const char *will_topic,
                                                    const char *will_payload,
                                                    uint32_t session_expiry = 0,
                                                    uint32_t will_delay = 0,
                                                    uint16_t keep_alive = 60,
                                                    bool clean_start = true)
{
    std::vector<uint8_t> body;
    const char proto[] = {'M', 'Q', 'T', 'T'};
    body.push_back(0x00);
    body.push_back(0x04);
    body.insert(body.end(), proto, proto + 4);
    body.push_back(0x05);
    body.push_back(static_cast<uint8_t>(0x04u | (clean_start ? 0x02u : 0x00u)));
    body.push_back(static_cast<uint8_t>((keep_alive >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(keep_alive & 0xFFu));
    if (session_expiry > 0 && will_delay > 0) {
        body.push_back(0x0A);
        body.push_back(SessionExpiryInterval);
        body.push_back(static_cast<uint8_t>((session_expiry >> 24) & 0xFFu));
        body.push_back(static_cast<uint8_t>((session_expiry >> 16) & 0xFFu));
        body.push_back(static_cast<uint8_t>((session_expiry >> 8) & 0xFFu));
        body.push_back(static_cast<uint8_t>(session_expiry & 0xFFu));
        body.push_back(WillDelayInterval);
        body.push_back(static_cast<uint8_t>((will_delay >> 24) & 0xFFu));
        body.push_back(static_cast<uint8_t>((will_delay >> 16) & 0xFFu));
        body.push_back(static_cast<uint8_t>((will_delay >> 8) & 0xFFu));
        body.push_back(static_cast<uint8_t>(will_delay & 0xFFu));
    } else if (session_expiry > 0) {
        append_u32_prop(body, SessionExpiryInterval, session_expiry);
    } else {
        body.push_back(0x00);
    }

    size_t cid_len = std::strlen(client_id);
    body.push_back(static_cast<uint8_t>((cid_len >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(cid_len & 0xFFu));
    body.insert(body.end(), client_id, client_id + cid_len);

    body.push_back(0x00);
    size_t tlen = std::strlen(will_topic);
    body.push_back(static_cast<uint8_t>((tlen >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(tlen & 0xFFu));
    body.insert(body.end(), will_topic, will_topic + tlen);
    size_t plen = std::strlen(will_payload);
    body.push_back(static_cast<uint8_t>((plen >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(plen & 0xFFu));
    body.insert(body.end(), will_payload, will_payload + plen);

    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(body.size()), rl, sizeof(rl));
    std::vector<uint8_t> pkt;
    pkt.push_back(0x10);
    pkt.insert(pkt.end(), rl, rl + rl_size);
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

static std::vector<uint8_t> build_disconnect()
{
    std::vector<uint8_t> pkt;
    pkt.push_back(0xE0);
    pkt.push_back(0x00);
    return pkt;
}

static std::vector<uint8_t> build_disconnect_with_reason(uint8_t reason)
{
    std::vector<uint8_t> pkt;
    pkt.push_back(0xE0);
    pkt.push_back(0x01);
    pkt.push_back(reason);
    return pkt;
}

static std::vector<uint8_t> build_publish_qos(const char *topic, const char *payload, uint8_t qos)
{
    std::vector<uint8_t> body;
    size_t tlen = std::strlen(topic);
    body.push_back(static_cast<uint8_t>((tlen >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(tlen & 0xFFu));
    body.insert(body.end(), topic, topic + tlen);
    if (qos > 0) {
        body.push_back(0x00);
        body.push_back(0x01);
    }
    size_t plen = std::strlen(payload);
    body.push_back(static_cast<uint8_t>((plen >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(plen & 0xFFu));
    body.insert(body.end(), payload, payload + plen);

    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(body.size()), rl, sizeof(rl));
    std::vector<uint8_t> pkt;
    pkt.push_back(static_cast<uint8_t>(0x30 | ((qos & 0x03u) << 1)));
    pkt.insert(pkt.end(), rl, rl + rl_size);
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

static std::vector<uint8_t> build_subscribe(uint16_t pid, const char *filter)
{
    std::vector<uint8_t> body;
    body.push_back(static_cast<uint8_t>((pid >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(pid & 0xFFu));
    body.push_back(0x00);
    size_t flen = std::strlen(filter);
    body.push_back(static_cast<uint8_t>((flen >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(flen & 0xFFu));
    body.insert(body.end(), filter, filter + flen);
    body.push_back(0x00);
    uint8_t rl[4];
    size_t rl_size = varint_encode(static_cast<uint32_t>(body.size()), rl, sizeof(rl));
    std::vector<uint8_t> pkt;
    pkt.push_back(0x82);
    pkt.insert(pkt.end(), rl, rl + rl_size);
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

static void pump(Broker &broker, uint16_t tid, MockLink &link)
{
    broker.on_readable(tid);
    broker.drain_tx(tid);
}

static bool connack_session_present(const MockLink &link)
{
    for (size_t i = 0; i + 3 < link.rx.size(); ++i) {
        if (link.rx[i] == 0x20) {
            return (link.rx[i + 2] & 0x01u) != 0;
        }
    }
    return false;
}

static void test_assigned_client_id()
{
    Broker broker;
    MockLink link = {{}, {}, 0, true};
    uint16_t tid = broker.attach_transport({&link, {mock_read, mock_write, mock_close}, true});
    link.tx = build_connect("", true);
    pump(broker, tid, link);
    assert(!link.rx.empty());
    assert(link.rx[0] == 0x20);
    bool has_assigned = false;
    for (size_t i = 0; i < link.rx.size(); ++i) {
        if (link.rx[i] == AssignedClientIdentifier) {
            has_assigned = true;
            break;
        }
    }
    assert(has_assigned);
    assert(broker.debug_find_session("nb-0-1") >= 0 ||
           broker.debug_find_session("nb-1-1") >= 0);
    assert(broker.connected_count() == 1);
}

static void test_session_present_on_resume()
{
    Broker broker;
    MockLink a = {{}, {}, 0, true};
    MockLink b = {{}, {}, 0, true};
    uint16_t ta = broker.attach_transport({&a, {mock_read, mock_write, mock_close}, true});
    uint16_t tb = broker.attach_transport({&b, {mock_read, mock_write, mock_close}, true});

    a.tx = build_connect("resume", false, 60);
    pump(broker, ta, a);
    a.tx = build_subscribe(1, "s/#");
    a.read_off = 0;
    pump(broker, ta, a);

    mock_close(&a);
    a.open = false;
    broker.detach_transport(ta);

    b.tx = build_connect("resume", false, 60);
    pump(broker, tb, b);
    assert(connack_session_present(b));
}

static void test_duplicate_takeover()
{
    Broker broker;
    MockLink old_c = {{}, {}, 0, true};
    MockLink new_c = {{}, {}, 0, true};
    uint16_t old_tid = broker.attach_transport({&old_c, {mock_read, mock_write, mock_close}, true});
    uint16_t new_tid = broker.attach_transport({&new_c, {mock_read, mock_write, mock_close}, true});

    old_c.tx = build_connect("dup");
    pump(broker, old_tid, old_c);
    new_c.tx = build_connect("dup");
    pump(broker, new_tid, new_c);

    assert(!old_c.open);
    assert(new_c.open);
    assert(rx_has_disconnect_reason(old_c, static_cast<uint8_t>(ReasonCode::SessionTakenOver)));
    assert(broker.metrics().session_takeovers == 1);
}

// MQTT 5 §3.1.4-3 / [MQTT-3.1.4-3]: takeover sends DISCONNECT 0x8E and closes the old
// connection; Will handling follows §3.1.2.5 / [MQTT-3.1.3-9] and §3.1.3.2.2.
static void session_takeover_will_behavior()
{
    // [MQTT-3.1.3-9]: Clean Start 0 + Will Delay > 0 — Will MUST NOT be sent on takeover.
    {
        Broker broker;
        MockLink old_c = {{}, {}, 0, true};
        MockLink new_c = {{}, {}, 0, true};
        MockLink watcher = {{}, {}, 0, true};
        uint16_t old_tid =
            broker.attach_transport({&old_c, {mock_read, mock_write, mock_close}, true});
        uint16_t new_tid =
            broker.attach_transport({&new_c, {mock_read, mock_write, mock_close}, true});
        uint16_t watch_tid =
            broker.attach_transport({&watcher, {mock_read, mock_write, mock_close}, true});

        old_c.tx = build_connect_with_will("take-delay", "will/take-delay", "delayed", 100, 30,
                                           60, false);
        pump(broker, old_tid, old_c);
        watcher.tx = build_connect("take-delay-watch");
        pump(broker, watch_tid, watcher);
        watcher.rx.clear();
        watcher.tx = build_subscribe(1, "will/take-delay");
        watcher.read_off = 0;
        pump(broker, watch_tid, watcher);

        new_c.tx = build_connect("take-delay", false, 100);
        pump(broker, new_tid, new_c);
        broker.drain_tx(watch_tid);

        assert(rx_has_disconnect_reason(old_c, static_cast<uint8_t>(ReasonCode::SessionTakenOver)));
        assert(broker.metrics().will_published == 0);
        assert(!rx_has_publish(watcher));
    }

    // §3.1.3.2.2 / [MQTT-3.1.4-3]: Clean Start 0 + Will Delay 0 — Will is published.
    {
        Broker broker;
        MockLink old_c = {{}, {}, 0, true};
        MockLink new_c = {{}, {}, 0, true};
        MockLink watcher = {{}, {}, 0, true};
        uint16_t old_tid =
            broker.attach_transport({&old_c, {mock_read, mock_write, mock_close}, true});
        uint16_t new_tid =
            broker.attach_transport({&new_c, {mock_read, mock_write, mock_close}, true});
        uint16_t watch_tid =
            broker.attach_transport({&watcher, {mock_read, mock_write, mock_close}, true});

        old_c.tx = build_connect_with_will("take-zero", "will/take-zero", "now", 0, 0, 60, false);
        pump(broker, old_tid, old_c);
        watcher.tx = build_connect("take-zero-watch");
        pump(broker, watch_tid, watcher);
        watcher.rx.clear();
        watcher.tx = build_subscribe(1, "will/take-zero");
        watcher.read_off = 0;
        pump(broker, watch_tid, watcher);

        new_c.tx = build_connect("take-zero", false);
        pump(broker, new_tid, new_c);
        broker.drain_tx(watch_tid);

        assert(rx_has_disconnect_reason(old_c, static_cast<uint8_t>(ReasonCode::SessionTakenOver)));
        assert(broker.metrics().will_published == 1);
        assert(rx_has_publish(watcher));
    }

    // §3.1.3.2.2: Clean Start 1 — session ends; stored Will is published on takeover.
    {
        Broker broker;
        MockLink old_c = {{}, {}, 0, true};
        MockLink new_c = {{}, {}, 0, true};
        MockLink watcher = {{}, {}, 0, true};
        uint16_t old_tid =
            broker.attach_transport({&old_c, {mock_read, mock_write, mock_close}, true});
        uint16_t new_tid =
            broker.attach_transport({&new_c, {mock_read, mock_write, mock_close}, true});
        uint16_t watch_tid =
            broker.attach_transport({&watcher, {mock_read, mock_write, mock_close}, true});

        old_c.tx = build_connect_with_will("take-clean", "will/take-clean", "end");
        pump(broker, old_tid, old_c);
        watcher.tx = build_connect("take-clean-watch");
        pump(broker, watch_tid, watcher);
        watcher.rx.clear();
        watcher.tx = build_subscribe(1, "will/take-clean");
        watcher.read_off = 0;
        pump(broker, watch_tid, watcher);

        new_c.tx = build_connect("take-clean", true);
        pump(broker, new_tid, new_c);
        broker.drain_tx(watch_tid);

        assert(rx_has_disconnect_reason(old_c, static_cast<uint8_t>(ReasonCode::SessionTakenOver)));
        assert(broker.metrics().will_published == 1);
        assert(rx_has_publish(watcher));
    }

    printf("PASS session_takeover_will_behavior\n");
}

static void test_invalid_duplicate_keeps_old()
{
    Broker broker;
    MockLink old_c = {{}, {}, 0, true};
    MockLink bad = {{}, {}, 0, true};
    uint16_t old_tid = broker.attach_transport({&old_c, {mock_read, mock_write, mock_close}, true});
    uint16_t bad_tid = broker.attach_transport({&bad, {mock_read, mock_write, mock_close}, true});

    old_c.tx = build_connect("stay");
    pump(broker, old_tid, old_c);
    bad.tx = build_connect_with_will("stay", "", "x");
    pump(broker, bad_tid, bad);

    assert(old_c.open);
    assert(!bad.open);
    assert(broker.connected_count() == 1);
}

static void test_offline_queue_drop_new()
{
    OfflineQueue q({4});
    PendingDelivery pd = {};
    for (int i = 0; i < 5; ++i) {
        if (i < 4) {
            assert(q.enqueue(pd));
        } else {
            assert(!q.enqueue(pd));
        }
    }
}

static bool rx_has_publish(const MockLink &link)
{
    for (size_t i = 0; i < link.rx.size(); ++i) {
        if (link.rx[i] == 0x30) {
            return true;
        }
    }
    return false;
}

static bool rx_has_disconnect_reason(const MockLink &link, uint8_t reason)
{
    for (size_t i = 0; i + 2 < link.rx.size(); ++i) {
        if (link.rx[i] == 0xE0 && link.rx[i + 1] == 0x01 && link.rx[i + 2] == reason) {
            return true;
        }
    }
    return false;
}

static void abrupt_disconnect_publishes_will()
{
    Broker broker;
    MockLink will_c = {{}, {}, 0, true};
    MockLink watcher = {{}, {}, 0, true};
    uint16_t will_tid = broker.attach_transport({&will_c, {mock_read, mock_write, mock_close}, true});
    uint16_t watch_tid =
        broker.attach_transport({&watcher, {mock_read, mock_write, mock_close}, true});

    will_c.tx = build_connect_with_will("will_src", "will/topic", "gone");
    pump(broker, will_tid, will_c);
    watcher.tx = build_connect("will_watch");
    pump(broker, watch_tid, watcher);
    watcher.rx.clear();
    watcher.tx = build_subscribe(1, "will/topic");
    watcher.read_off = 0;
    pump(broker, watch_tid, watcher);

    broker.detach_transport(will_tid);
    broker.drain_tx(watch_tid);
    assert(broker.metrics().will_published == 1);
    assert(rx_has_publish(watcher));
    printf("PASS abrupt_disconnect_publishes_will\n");
}

static void disconnect_0x04_publishes_once()
{
    Broker broker;
    MockLink will_c = {{}, {}, 0, true};
    MockLink watcher = {{}, {}, 0, true};
    uint16_t will_tid = broker.attach_transport({&will_c, {mock_read, mock_write, mock_close}, true});
    uint16_t watch_tid =
        broker.attach_transport({&watcher, {mock_read, mock_write, mock_close}, true});

    will_c.tx = build_connect_with_will("once", "will/once", "bye");
    pump(broker, will_tid, will_c);
    watcher.tx = build_connect("once_watch");
    pump(broker, watch_tid, watcher);
    watcher.rx.clear();
    watcher.tx = build_subscribe(1, "will/once");
    watcher.read_off = 0;
    pump(broker, watch_tid, watcher);

    will_c.tx = build_disconnect_with_reason(0x04);
    will_c.read_off = 0;
    pump(broker, will_tid, will_c);
    broker.drain_tx(watch_tid);
    assert(broker.metrics().will_published == 1);
    assert(rx_has_publish(watcher));

    broker.tick(1000);
    broker.drain_tx(watch_tid);
    assert(broker.metrics().will_published == 1);
    assert(!broker.debug_any_will_scheduled());
    printf("PASS disconnect_0x04_publishes_once\n");
}

static void disconnect_other_graceful_reason_suppresses_will()
{
    Broker broker;
    MockLink will_c = {{}, {}, 0, true};
    MockLink watcher = {{}, {}, 0, true};
    uint16_t will_tid = broker.attach_transport({&will_c, {mock_read, mock_write, mock_close}, true});
    uint16_t watch_tid =
        broker.attach_transport({&watcher, {mock_read, mock_write, mock_close}, true});

    will_c.tx = build_connect_with_will("other", "will/other", "nope");
    pump(broker, will_tid, will_c);
    watcher.tx = build_connect("other_watch");
    pump(broker, watch_tid, watcher);
    watcher.rx.clear();
    watcher.tx = build_subscribe(1, "will/other");
    watcher.read_off = 0;
    pump(broker, watch_tid, watcher);

    // 0x0B = Implementation specific — graceful, not 0x04.
    will_c.tx = build_disconnect_with_reason(0x0B);
    will_c.read_off = 0;
    pump(broker, will_tid, will_c);
    broker.drain_tx(watch_tid);
    assert(broker.metrics().will_published == 0);
    assert(!rx_has_publish(watcher));
    printf("PASS disconnect_other_graceful_reason_suppresses_will\n");
}

static void keepalive_timeout_publishes_will()
{
    Broker broker;
    MockLink will_c = {{}, {}, 0, true};
    MockLink watcher = {{}, {}, 0, true};
    uint16_t will_tid = broker.attach_transport({&will_c, {mock_read, mock_write, mock_close}, true});
    uint16_t watch_tid =
        broker.attach_transport({&watcher, {mock_read, mock_write, mock_close}, true});

    will_c.tx = build_connect_with_will("katime", "will/katime", "timeout", 0, 0, 10);
    pump(broker, will_tid, will_c);
    watcher.tx = build_connect("katime_watch");
    pump(broker, watch_tid, watcher);
    watcher.rx.clear();
    watcher.tx = build_subscribe(1, "will/katime");
    watcher.read_off = 0;
    pump(broker, watch_tid, watcher);

    broker.tick(16);  // 1.5 × 10 s keep-alive
    broker.drain_tx(watch_tid);
    assert(broker.metrics().keepalive_disconnects == 1);
    assert(broker.metrics().will_published == 1);
    assert(rx_has_publish(watcher));
    printf("PASS keepalive_timeout_publishes_will\n");
}

static void qos0_offline_not_queued()
{
    Broker broker;
    MockLink pub = {{}, {}, 0, true};
    MockLink sub = {{}, {}, 0, true};
    uint16_t pub_tid = broker.attach_transport({&pub, {mock_read, mock_write, mock_close}, true});
    uint16_t sub_tid = broker.attach_transport({&sub, {mock_read, mock_write, mock_close}, true});

    sub.tx = build_connect("offline-sub", false, 3600);
    pump(broker, sub_tid, sub);
    sub.tx = build_subscribe(1, "offline/#");
    sub.read_off = 0;
    pump(broker, sub_tid, sub);
    int16_t slot = broker.debug_find_session("offline-sub");
    assert(slot >= 0);

    broker.detach_transport(sub_tid);
    assert(broker.debug_offline_queue_depth(static_cast<uint16_t>(slot)) == 0);

    pub.tx = build_connect("offline-pub");
    pump(broker, pub_tid, pub);
    pub.tx = build_publish_qos("offline/qos0", "drop-me", 0);
    pub.read_off = 0;
    pump(broker, pub_tid, pub);
    assert(broker.debug_offline_queue_depth(static_cast<uint16_t>(slot)) == 0);

    pub.tx = build_publish_qos("offline/qos1", "keep-me", 1);
    pub.read_off = 0;
    pump(broker, pub_tid, pub);
    assert(broker.debug_offline_queue_depth(static_cast<uint16_t>(slot)) == 1);
    printf("PASS qos0_offline_not_queued\n");
}

static void graceful_disconnect_suppresses_will()
{
    Broker broker;
    MockLink will_c = {{}, {}, 0, true};
    MockLink watcher = {{}, {}, 0, true};
    uint16_t will_tid = broker.attach_transport({&will_c, {mock_read, mock_write, mock_close}, true});
    uint16_t watch_tid =
        broker.attach_transport({&watcher, {mock_read, mock_write, mock_close}, true});

    will_c.tx = build_connect_with_will("grace", "will/grace", "nope");
    pump(broker, will_tid, will_c);
    watcher.tx = build_connect("grace_watch");
    pump(broker, watch_tid, watcher);
    watcher.rx.clear();
    watcher.tx = build_subscribe(1, "will/grace");
    watcher.read_off = 0;
    pump(broker, watch_tid, watcher);

    will_c.tx = build_disconnect();
    will_c.read_off = 0;
    pump(broker, will_tid, will_c);
    broker.drain_tx(watch_tid);
    assert(broker.metrics().will_published == 0);
    assert(!rx_has_publish(watcher));
    printf("PASS disconnect_0x00_suppresses_will\n");
}

static void test_session_expiry_ticks_end_session()
{
    Broker broker;
    MockLink link = {{}, {}, 0, true};
    uint16_t tid = broker.attach_transport({&link, {mock_read, mock_write, mock_close}, true});
    link.tx = build_connect("expiry", false, 50);
    pump(broker, tid, link);
    link.tx = build_subscribe(1, "exp/#");
    link.read_off = 0;
    pump(broker, tid, link);
    broker.detach_transport(tid);
    assert(broker.debug_find_session("expiry") >= 0);

    broker.tick(49);
    assert(broker.debug_find_session("expiry") >= 0);
    broker.tick(50);
    assert(broker.debug_find_session("expiry") < 0);
}

static void test_will_delay_before_session_expiry()
{
    Broker broker;
    MockLink will_c = {{}, {}, 0, true};
    MockLink watcher = {{}, {}, 0, true};
    uint16_t will_tid = broker.attach_transport({&will_c, {mock_read, mock_write, mock_close}, true});
    uint16_t watch_tid =
        broker.attach_transport({&watcher, {mock_read, mock_write, mock_close}, true});

    will_c.tx = build_connect_with_will("delay", "will/delay", "late", 100, 30);
    pump(broker, will_tid, will_c);
    watcher.tx = build_connect("delay_watch");
    pump(broker, watch_tid, watcher);
    watcher.rx.clear();
    watcher.tx = build_subscribe(1, "will/delay");
    watcher.read_off = 0;
    pump(broker, watch_tid, watcher);

    broker.detach_transport(will_tid);
    broker.tick(29);
    broker.drain_tx(watch_tid);
    assert(broker.metrics().will_published == 0);
    assert(!rx_has_publish(watcher));

    broker.tick(30);
    broker.drain_tx(watch_tid);
    assert(broker.metrics().will_published == 1);
    assert(rx_has_publish(watcher));
    printf("PASS will_delay_still_honored\n");
}

static void test_retained_store_byte_budget()
{
    MessagePool pool({512, 32, 40});
    TopicInternPool intern({32, 4096, 128});
    RetainedStore store({4, 20});

    TopicHandle t1 = intern.intern("r/a");
    TopicHandle t2 = intern.intern("r/b");
    MessageHandle m1 = pool.acquire_empty();
    MessageHandle m2 = pool.acquire_empty();
    uint8_t small[6] = {'s', 'm', 'a', 'l', 'l', '!'};
    uint8_t big[16] = {'b', 'i', 'g', ' ', 'p', 'a', 'y', 'l', 'o', 'a', 'd', ' ', 'x', 'x', 'x', '!'};
    assert(pool.append_payload(m1, small, sizeof(small)));
    assert(pool.append_payload(m2, big, sizeof(big)));

    assert(store.put(t1, m1, &pool, &intern));
    assert(store.bytes_used() == 6);
    assert(!store.put(t2, m2, &pool, &intern));
    assert(store.entry_count() == 1);

    MessageHandle m1b = pool.acquire_empty();
    uint8_t too_big[21];
    for (size_t i = 0; i < sizeof(too_big); ++i) {
        too_big[i] = static_cast<uint8_t>('o');
    }
    assert(pool.append_payload(m1b, too_big, sizeof(too_big)));
    assert(!store.put(t1, m1b, &pool, &intern));
    assert(store.bytes_used() == 6);
}

static void test_session_ends_when_expiry_zero()
{
    Broker broker;
    MockLink link = {{}, {}, 0, true};
    uint16_t tid = broker.attach_transport({&link, {mock_read, mock_write, mock_close}, true});
    link.tx = build_connect("end_zero");
    pump(broker, tid, link);
    link.tx = build_subscribe(1, "z/#");
    link.read_off = 0;
    pump(broker, tid, link);
    assert(broker.debug_find_session("end_zero") >= 0);

    mock_close(&link);
    broker.on_readable(tid);
    assert(broker.debug_find_session("end_zero") < 0);
}

static void test_broker_offline_quota_drop()
{
    Broker broker;
    MockLink link = {{}, {}, 0, true};
    uint16_t tid = broker.attach_transport({&link, {mock_read, mock_write, mock_close}, true});
    link.tx = build_connect("offline");
    pump(broker, tid, link);
    int16_t slot = broker.debug_find_session("offline");
    assert(slot >= 0);

    assert(broker.debug_fill_offline_queue(static_cast<uint16_t>(slot),
                                           static_cast<size_t>(BrokerLimits::OfflineQueuePerClient)));
    assert(!broker.debug_fill_offline_queue(static_cast<uint16_t>(slot), 1));
    assert(broker.metrics().quota_drop_count >= 1);
}

int main()
{
    test_assigned_client_id();
    test_session_present_on_resume();
    test_duplicate_takeover();
    session_takeover_will_behavior();
    test_invalid_duplicate_keeps_old();
    test_offline_queue_drop_new();
    test_broker_offline_quota_drop();
    abrupt_disconnect_publishes_will();
    graceful_disconnect_suppresses_will();
    disconnect_0x04_publishes_once();
    disconnect_other_graceful_reason_suppresses_will();
    keepalive_timeout_publishes_will();
    test_will_delay_before_session_expiry();
    qos0_offline_not_queued();
    test_session_expiry_ticks_end_session();
    test_retained_store_byte_budget();
    test_session_ends_when_expiry_zero();
    return 0;
}