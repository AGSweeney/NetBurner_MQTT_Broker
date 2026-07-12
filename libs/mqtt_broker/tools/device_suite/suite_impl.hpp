// Conformance tests, benchmarks, and JSON writer for mqtt_device_suite.
#pragma once

#include "mqtt_client.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace device_suite {
namespace suite {

using Clock = std::chrono::steady_clock;

struct TestResult {
    std::string name;
    std::string version;
    bool pass = false;
    std::string detail;
    double ms = 0;
};

struct ThroughputResult {
    std::string version;
    int published = 0;
    int received = 0;
    int publish_rate = 0;
    int e2e_rate = 0;
    double publish_s = 0;
    double e2e_s = 0;
};

struct LatencyResult {
    std::string version;
    int samples = 0;
    double p50_ms = 0;
    double p95_ms = 0;
    double p99_ms = 0;
    double min_ms = 0;
    double max_ms = 0;
    double mean_ms = 0;
    std::vector<double> raw_ms;
};

struct MetricsSnap {
    int parser_errors = 0;
    int slow_consumer_disconnects = 0;
    int pool_exhaustion = 0;
    int dropped_quota = 0;
    int dropped_too_large = 0;
    int keepalive_disconnects = 0;
};

struct SoakResult {
    std::string version;
    double duration_s = 0;
    int published = 0;
    int received = 0;
    int publish_rate = 0;
    double delivery_pct = 0;
    std::vector<std::array<int, 3>> per_second;
    MetricsSnap health_delta;
};

struct Config {
    std::string host = "172.16.82.8";
    uint16_t port = 1883;
    std::string out = "mqtt_conformance_results.json";
    int bench_count = 5000;
    int lat_count = 200;
    double soak_seconds = 0;
    bool send_only = false;
    std::string send_topic = "bench/manual/seq";
    int send_count = 5000;
    int send_delay_ms = 0;
    int mqtt_version = 4;
};

inline std::string ver_label(int v) { return v == 4 ? "v3.1.1" : "v5"; }

inline double elapsed_ms(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

inline void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

inline bool pkt_eq(const std::vector<uint8_t> &a, std::initializer_list<uint8_t> b)
{
    if (a.size() != b.size()) {
        return false;
    }
    size_t i = 0;
    for (uint8_t x : b) {
        if (a[i++] != x) {
            return false;
        }
    }
    return true;
}

inline std::vector<uint8_t> bytes(const std::string &s)
{
    return {s.begin(), s.end()};
}

inline TestResult run_test(const std::string &name, const std::string &version,
                           const std::function<void()> &fn)
{
    TestResult r{name, version, false, "", 0};
    auto t0 = Clock::now();
    try {
        fn();
        r.pass = true;
    } catch (const std::exception &ex) {
        r.detail = ex.what();
    }
    r.ms = elapsed_ms(t0);
    std::cout << "  " << (r.pass ? "PASS" : "FAIL") << " [" << version << "] " << name;
    if (!r.pass) {
        std::cout << " - " << r.detail;
    }
    std::cout << "\n";
    return r;
}

inline std::string http_get(const std::string &host, const std::string &path)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), "80", &hints, &res) != 0 || !res) {
        return {};
    }
    socket_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == kInvalidSocket) {
        freeaddrinfo(res);
        return {};
    }
    if (::connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
        close_socket(s);
        freeaddrinfo(res);
        return {};
    }
    freeaddrinfo(res);
    std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\n\r\n";
    ::send(s, req.c_str(), static_cast<int>(req.size()), 0);
    std::string resp;
    char buf[4096];
    int n;
    while ((n = ::recv(s, buf, sizeof(buf), 0)) > 0) {
        resp.append(buf, static_cast<size_t>(n));
    }
    close_socket(s);
    const auto pos = resp.find("\r\n\r\n");
    return pos == std::string::npos ? std::string{} : resp.substr(pos + 4);
}

inline int json_metric(const std::string &body, const char *key)
{
    const std::string needle = std::string("\"") + key + "\":";
    auto p = body.find(needle);
    if (p == std::string::npos) {
        return 0;
    }
    return std::atoi(body.c_str() + static_cast<int>(p + needle.size()));
}

inline MetricsSnap fetch_metrics(const std::string &host)
{
    MetricsSnap m;
    const std::string body = http_get(host, "/api/broker/status");
    if (body.empty()) {
        return m;
    }
    m.parser_errors = json_metric(body, "parser_errors");
    m.slow_consumer_disconnects = json_metric(body, "slow_consumer_disconnects");
    m.pool_exhaustion = json_metric(body, "pool_exhaustion");
    m.dropped_quota = json_metric(body, "dropped_quota");
    m.dropped_too_large = json_metric(body, "dropped_too_large");
    m.keepalive_disconnects = json_metric(body, "keepalive_disconnects");
    return m;
}

inline MetricsSnap diff_metrics(const MetricsSnap &after, const MetricsSnap &before)
{
    MetricsSnap d;
    d.parser_errors = after.parser_errors - before.parser_errors;
    d.slow_consumer_disconnects = after.slow_consumer_disconnects - before.slow_consumer_disconnects;
    d.pool_exhaustion = after.pool_exhaustion - before.pool_exhaustion;
    d.dropped_quota = after.dropped_quota - before.dropped_quota;
    d.dropped_too_large = after.dropped_too_large - before.dropped_too_large;
    d.keepalive_disconnects = after.keepalive_disconnects - before.keepalive_disconnects;
    return d;
}

inline std::vector<TestResult> make_tests(const Config &cfg, int v)
{
    const std::string tag = "v" + std::to_string(v);
    const std::string vl = ver_label(v);
    std::vector<TestResult> out;
    auto add = [&](const std::string &name, auto fn) { out.push_back(run_test(name, vl, fn)); };

    add("connect", [&] {
        MqttClient c(cfg.host, cfg.port, v, "conf-" + tag + "-conn");
        auto ca = c.connect_mqtt();
        c.disconnect();
        if (v == 4) {
            if (!pkt_eq(ca, {0x20, 0x02, 0x00, 0x00})) {
                throw std::runtime_error("bad CONNACK");
            }
        } else if (ca.size() < 4 || ca[0] != 0x20 || ca[3] != 0x00) {
            throw std::runtime_error("bad CONNACK");
        }
    });

    add("ping", [&] {
        MqttClient c(cfg.host, cfg.port, v, "conf-" + tag + "-ping");
        c.connect_mqtt();
        auto p = c.ping_recv();
        c.disconnect();
        if (!pkt_eq(p, {0xD0, 0x00})) {
            throw std::runtime_error("bad PINGRESP");
        }
    });

    add("qos0 pub/sub", [&] {
        MqttClient sub(cfg.host, cfg.port, v, "conf-" + tag + "-q0s");
        MqttClient pub(cfg.host, cfg.port, v, "conf-" + tag + "-q0p");
        sub.connect_mqtt();
        pub.connect_mqtt();
        sub.subscribe("conf/" + tag + "/q0", 0);
        pub.publish("conf/" + tag + "/q0", bytes("payload-q0"));
        auto m = sub.recv_publish();
        pub.disconnect();
        sub.disconnect();
        if (m.payload != bytes("payload-q0") || m.qos != 0) {
            throw std::runtime_error("qos0 mismatch");
        }
    });

    add("qos1 pub/sub + PUBACK", [&] {
        MqttClient sub(cfg.host, cfg.port, v, "conf-" + tag + "-q1s");
        MqttClient pub(cfg.host, cfg.port, v, "conf-" + tag + "-q1p");
        sub.connect_mqtt();
        pub.connect_mqtt();
        sub.subscribe("conf/" + tag + "/q1", 1);
        pub.publish("conf/" + tag + "/q1", bytes("payload-q1"), 1, 0x0101);
        auto ack = pub.recv_packet();
        if (ack.empty() || ack[0] != 0x40) {
            throw std::runtime_error("no PUBACK");
        }
        if (v == 4 && !pkt_eq(ack, {0x40, 0x02, 0x01, 0x01})) {
            throw std::runtime_error("bad PUBACK");
        }
        auto m = sub.recv_publish();
        pub.disconnect();
        sub.disconnect();
        if (m.payload != bytes("payload-q1") || m.qos != 1) {
            throw std::runtime_error("qos1 mismatch");
        }
    });

    add("qos2 full handshake", [&] {
        MqttClient sub(cfg.host, cfg.port, v, "conf-" + tag + "-q2s");
        MqttClient pub(cfg.host, cfg.port, v, "conf-" + tag + "-q2p");
        sub.connect_mqtt();
        pub.connect_mqtt();
        sub.subscribe("conf/" + tag + "/q2", 2);
        pub.publish("conf/" + tag + "/q2", bytes("payload-q2"), 2, 0x0202);
        auto rec = pub.recv_packet();
        if (rec.empty() || rec[0] != 0x50) {
            throw std::runtime_error("no PUBREC");
        }
        pub.send_ack(6, 0x0202);
        auto comp = pub.recv_packet();
        if (comp.empty() || comp[0] != 0x70) {
            throw std::runtime_error("no PUBCOMP");
        }
        auto m = sub.recv_publish();
        try {
            sub.recv_publish(1000);
        } catch (...) {
        }
        pub.disconnect();
        sub.disconnect();
        if (m.payload != bytes("payload-q2") || m.qos != 2) {
            throw std::runtime_error("qos2 mismatch");
        }
    });

    add("retained set/deliver/delete", [&] {
        const std::string topic = "conf/" + tag + "/ret";
        MqttClient pub(cfg.host, cfg.port, v, "conf-" + tag + "-retp");
        pub.connect_mqtt();
        pub.publish(topic, bytes("retained-msg"), 0, 0, true);
        sleep_ms(300);
        MqttClient sub(cfg.host, cfg.port, v, "conf-" + tag + "-rets");
        sub.connect_mqtt();
        sub.subscribe(topic, 0);
        auto m = sub.recv_publish();
        pub.publish(topic, {}, 0, 0, true);
        sleep_ms(300);
        MqttClient sub2(cfg.host, cfg.port, v, "conf-" + tag + "-rets2");
        sub2.connect_mqtt();
        sub2.subscribe(topic, 0);
        bool stale = false;
        try {
            sub2.recv_publish(1000);
            stale = true;
        } catch (...) {
        }
        pub.disconnect();
        sub.disconnect();
        sub2.disconnect();
        if (m.payload != bytes("retained-msg") || !m.retain || stale) {
            throw std::runtime_error("retained lifecycle failed");
        }
    });

    add("will on abrupt disconnect", [&] {
        const std::string topic = "conf/" + tag + "/will";
        MqttClient sub(cfg.host, cfg.port, v, "conf-" + tag + "-wills");
        sub.connect_mqtt();
        sub.subscribe(topic, 0);
        MqttClient dev(cfg.host, cfg.port, v, "conf-" + tag + "-willd");
        const auto will = bytes("device-died");
        dev.connect_mqtt(30, true, 0, &topic, &will);
        dev.abrupt_close();
        auto m = sub.recv_publish(8000);
        sub.disconnect();
        if (m.payload != will) {
            throw std::runtime_error("will not delivered");
        }
    });

    add("wildcards + and #", [&] {
        MqttClient sub(cfg.host, cfg.port, v, "conf-" + tag + "-wild");
        MqttClient pub(cfg.host, cfg.port, v, "conf-" + tag + "-wildp");
        sub.connect_mqtt();
        pub.connect_mqtt();
        sub.subscribe("conf/" + tag + "/wc/+/x", 0, 1);
        sub.subscribe("conf/" + tag + "/deep/#", 0, 2);
        pub.publish("conf/" + tag + "/wc/a/x", bytes("plus"));
        auto m1 = sub.recv_publish();
        pub.publish("conf/" + tag + "/deep/a/b/c", bytes("hash"));
        auto m2 = sub.recv_publish();
        pub.disconnect();
        sub.disconnect();
        if (m1.payload != bytes("plus") || m2.payload != bytes("hash")) {
            throw std::runtime_error("wildcard mismatch");
        }
    });

    add("unsubscribe stops delivery", [&] {
        MqttClient sub(cfg.host, cfg.port, v, "conf-" + tag + "-unsub");
        MqttClient pub(cfg.host, cfg.port, v, "conf-" + tag + "-unsubp");
        sub.connect_mqtt();
        pub.connect_mqtt();
        sub.subscribe("conf/" + tag + "/us", 0);
        auto ua = sub.unsubscribe("conf/" + tag + "/us");
        if (ua.empty() || ua[0] != 0xB0) {
            throw std::runtime_error("no UNSUBACK");
        }
        if (v == 4 && ua.size() != 4) {
            throw std::runtime_error("3.1.1 UNSUBACK wrong size");
        }
        pub.publish("conf/" + tag + "/us", bytes("should-not-arrive"));
        bool leaked = false;
        try {
            sub.recv_publish(1000);
            leaked = true;
        } catch (...) {
        }
        pub.disconnect();
        sub.disconnect();
        if (leaked) {
            throw std::runtime_error("delivery after UNSUBSCRIBE");
        }
    });

    add("session persistence", [&] {
        const std::string cid = "conf-" + tag + "-persist";
        MqttClient c1(cfg.host, cfg.port, v, cid);
        if (v == 4) {
            c1.connect_mqtt(30, false);
        } else {
            c1.connect_mqtt(30, true, 120);
        }
        c1.subscribe("conf/" + tag + "/keep", 1);
        c1.abrupt_close();
        sleep_ms(1000);
        MqttClient c2(cfg.host, cfg.port, v, cid);
        auto ca = c2.connect_raw(30, false);
        if (ca.empty() || ca[0] != 0x20) {
            throw std::runtime_error("reconnect CONNACK failed");
        }
        const bool session_present = (ca.size() > 2) && (ca[2] & 0x01);
        c2.disconnect();
        if (!session_present) {
            throw std::runtime_error("session lost");
        }
    });

    add("keep-alive enforcement", [&] {
        MqttClient c(cfg.host, cfg.port, v, "conf-" + tag + "-ka");
        c.connect_mqtt(3);
        sleep_ms(6500);
        bool closed = false;
        try {
            auto pkt = c.recv_packet(4000);
            closed = pkt.empty() || (v == 5 && !pkt.empty() && pkt[0] == 0xE0);
        } catch (...) {
            closed = true;
        }
        c.close();
        if (!closed) {
            throw std::runtime_error("broker did not enforce keep-alive");
        }
    });

    add("4KB payload integrity", [&] {
        std::vector<uint8_t> big(4096);
        for (size_t i = 0; i < big.size(); ++i) {
            big[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
        }
        MqttClient sub(cfg.host, cfg.port, v, "conf-" + tag + "-big");
        MqttClient pub(cfg.host, cfg.port, v, "conf-" + tag + "-bigp");
        sub.connect_mqtt();
        pub.connect_mqtt();
        sub.subscribe("conf/" + tag + "/big", 0);
        pub.publish("conf/" + tag + "/big", big);
        auto m = sub.recv_publish(8000);
        pub.disconnect();
        sub.disconnect();
        if (m.payload != big) {
            throw std::runtime_error("payload corrupt");
        }
    });

    return out;
}

inline std::vector<TestResult> cross_version_tests(const Config &cfg)
{
    std::vector<TestResult> out;
    auto add = [&](const std::string &name, auto fn) { out.push_back(run_test(name, "cross", fn)); };

    add("MQTT5 pub -> 3.1.1 sub", [&] {
        MqttClient sub(cfg.host, cfg.port, 4, "conf-x-v4sub");
        MqttClient pub(cfg.host, cfg.port, 5, "conf-x-v5pub");
        sub.connect_mqtt();
        pub.connect_mqtt();
        sub.subscribe("conf/x/54", 0);
        pub.publish("conf/x/54", bytes("five-to-four"));
        auto m = sub.recv_publish();
        pub.disconnect();
        sub.disconnect();
        if (m.payload != bytes("five-to-four")) {
            throw std::runtime_error("payload mismatch");
        }
    });

    add("3.1.1 pub -> MQTT5 sub", [&] {
        MqttClient sub(cfg.host, cfg.port, 5, "conf-x-v5sub");
        MqttClient pub(cfg.host, cfg.port, 4, "conf-x-v4pub");
        sub.connect_mqtt();
        pub.connect_mqtt();
        sub.subscribe("conf/x/45", 0);
        pub.publish("conf/x/45", bytes("four-to-five"));
        auto m = sub.recv_publish();
        pub.disconnect();
        sub.disconnect();
        if (m.payload != bytes("four-to-five")) {
            throw std::runtime_error("payload mismatch");
        }
    });

    add("retained cross-version", [&] {
        MqttClient pub(cfg.host, cfg.port, 5, "conf-x-retp");
        pub.connect_mqtt();
        pub.publish("conf/x/ret", bytes("x-retained"), 0, 0, true);
        sleep_ms(300);
        MqttClient sub(cfg.host, cfg.port, 4, "conf-x-rets");
        sub.connect_mqtt();
        sub.subscribe("conf/x/ret", 0);
        auto m = sub.recv_publish();
        pub.publish("conf/x/ret", {}, 0, 0, true);
        pub.disconnect();
        sub.disconnect();
        if (m.payload != bytes("x-retained") || !m.retain) {
            throw std::runtime_error("retained cross failed");
        }
    });

    add("QoS1 cross-version", [&] {
        MqttClient sub(cfg.host, cfg.port, 4, "conf-x-q1sub");
        MqttClient pub(cfg.host, cfg.port, 5, "conf-x-q1pub");
        sub.connect_mqtt();
        pub.connect_mqtt();
        sub.subscribe("conf/x/q1", 1);
        pub.publish("conf/x/q1", bytes("x-qos1"), 1, 9);
        auto ack = pub.recv_packet();
        if (ack.empty() || ack[0] != 0x40) {
            throw std::runtime_error("no PUBACK");
        }
        auto m = sub.recv_publish();
        pub.disconnect();
        sub.disconnect();
        if (m.payload != bytes("x-qos1") || m.qos != 1) {
            throw std::runtime_error("qos1 cross failed");
        }
    });

    return out;
}

inline int count_publish_in_buffer(const std::vector<uint8_t> &buf, size_t &consumed)
{
    consumed = 0;
    if (buf.size() < 2) {
        return 0;
    }
    size_t i = 0;
    int count = 0;
    while (i < buf.size()) {
        if (i + 1 >= buf.size()) {
            break;
        }
        uint32_t rl = 0;
        uint32_t mul = 1;
        size_t j = i + 1;
        bool ok = false;
        while (j < buf.size()) {
            rl += static_cast<uint32_t>(buf[j] & 0x7F) * mul;
            mul *= 128;
            if (!(buf[j] & 0x80)) {
                ok = true;
                ++j;
                break;
            }
            ++j;
        }
        if (!ok || buf.size() < j + rl) {
            break;
        }
        if ((buf[i] >> 4) == 3) {
            ++count;
        }
        i = j + rl;
    }
    consumed = i;
    return count;
}

inline ThroughputResult bench_throughput(const Config &cfg, int v)
{
    const std::string tag = "v" + std::to_string(v);
    MqttClient sub(cfg.host, cfg.port, v, "bench-" + tag + "-sub");
    MqttClient pub(cfg.host, cfg.port, v, "bench-" + tag + "-pub");
    sub.connect_mqtt();
    pub.connect_mqtt();
    sub.subscribe("bench/" + tag + "/tp", 0);
    sub.set_recv_timeout_ms(500);

    struct State {
        std::atomic<int> received{0};
        double last_rx = 0;
    } state;
    std::atomic<bool> stop{false};

    std::thread reader([&] {
        std::vector<uint8_t> buf;
        while (!stop.load()) {
            char chunk[65536];
            int n = ::recv(sub.native_socket(), chunk, sizeof(chunk), 0);
            if (n <= 0) {
                if (n < 0) {
                    continue;
                }
                return;
            }
            buf.insert(buf.end(), chunk, chunk + n);
            size_t consumed = 0;
            int c = count_publish_in_buffer(buf, consumed);
            if (c > 0) {
                state.received.fetch_add(c);
                state.last_rx =
                    std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
            }
            if (consumed > 0) {
                buf.erase(buf.begin(), buf.begin() + static_cast<long>(consumed));
            }
        }
    });

    const auto payload = bytes(R"({"bench":"throughput","pad":")" + std::string(32, 'x') + "\"}");
    auto t0 = Clock::now();
    for (int i = 0; i < cfg.bench_count; ++i) {
        pub.publish("bench/" + tag + "/tp", payload);
    }
    double pub_elapsed =
        std::chrono::duration<double>(Clock::now() - t0).count();
    auto deadline = Clock::now() + std::chrono::seconds(30);
    int last = -1;
    while (Clock::now() < deadline && state.received.load() < cfg.bench_count) {
        int cur = state.received.load();
        if (cur != last) {
            last = cur;
            deadline = Clock::now() + std::chrono::seconds(5);
        }
        sleep_ms(100);
    }
    stop.store(true);
    reader.join();
    const int received = state.received.load();
    double t0s = std::chrono::duration<double>(t0.time_since_epoch()).count();
    double e2e_elapsed = (received > 0 && state.last_rx > t0s) ? (state.last_rx - t0s) : 0;
    pub.disconnect();
    sub.disconnect();

    ThroughputResult r;
    r.version = ver_label(v);
    r.published = cfg.bench_count;
    r.received = received;
    r.publish_s = pub_elapsed;
    r.e2e_s = e2e_elapsed;
    r.publish_rate = pub_elapsed > 0 ? static_cast<int>(cfg.bench_count / pub_elapsed) : 0;
    r.e2e_rate = e2e_elapsed > 0 ? static_cast<int>(received / e2e_elapsed) : 0;
    return r;
}

inline LatencyResult bench_latency(const Config &cfg, int v)
{
    const std::string tag = "v" + std::to_string(v);
    MqttClient sub(cfg.host, cfg.port, v, "lat-" + tag + "-sub");
    MqttClient pub(cfg.host, cfg.port, v, "lat-" + tag + "-pub");
    sub.connect_mqtt();
    pub.connect_mqtt();
    sub.subscribe("bench/" + tag + "/lat", 0);

    LatencyResult r;
    r.version = ver_label(v);
    r.samples = cfg.lat_count;
    for (int i = 0; i < cfg.lat_count; ++i) {
        std::vector<uint8_t> pl(4);
        pl[0] = static_cast<uint8_t>((i >> 24) & 0xFF);
        pl[1] = static_cast<uint8_t>((i >> 16) & 0xFF);
        pl[2] = static_cast<uint8_t>((i >> 8) & 0xFF);
        pl[3] = static_cast<uint8_t>(i & 0xFF);
        auto t0 = Clock::now();
        pub.publish("bench/" + tag + "/lat", pl);
        auto m = sub.recv_publish();
        r.raw_ms.push_back(elapsed_ms(t0));
        const int got = (static_cast<int>(m.payload[0]) << 24) |
                        (static_cast<int>(m.payload[1]) << 16) |
                        (static_cast<int>(m.payload[2]) << 8) | static_cast<int>(m.payload[3]);
        if (got != i) {
            throw std::runtime_error("latency seq mismatch");
        }
        sleep_ms(5);
    }
    pub.disconnect();
    sub.disconnect();
    std::sort(r.raw_ms.begin(), r.raw_ms.end());
    r.min_ms = r.raw_ms.front();
    r.max_ms = r.raw_ms.back();
    r.p50_ms = r.raw_ms[r.raw_ms.size() / 2];
    r.p95_ms = r.raw_ms[static_cast<size_t>(r.raw_ms.size() * 0.95)];
    r.p99_ms = r.raw_ms[static_cast<size_t>(r.raw_ms.size() * 0.99)];
    double sum = 0;
    for (double x : r.raw_ms) {
        sum += x;
    }
    r.mean_ms = sum / r.raw_ms.size();
    return r;
}

inline int run_send_only(const Config &cfg)
{
    const int v = (cfg.mqtt_version == 5) ? 5 : 4;
    const std::string tag = "v" + std::to_string(v);
    MetricsSnap m0 = fetch_metrics(cfg.host);

    std::cout << "=== Send-only (serialized) ===\n";
    std::cout << "  MQTT " << ver_label(v) << " publisher -> " << cfg.host << ":" << cfg.port << "\n";
    std::cout << "  topic: " << cfg.send_topic << "\n";
    std::cout << "  count: " << cfg.send_count;
    if (cfg.send_delay_ms > 0) {
        std::cout << ", delay: " << cfg.send_delay_ms << " ms";
    }
    std::cout << "\n";
    std::cout << "  In another terminal:\n";
    std::cout << "    mosquitto_sub -h " << cfg.host << " -p " << cfg.port << " -t \""
              << cfg.send_topic << "\" -v\n";
    std::cout << "  Payload format: seq=N (monotonic). Press Ctrl+C to stop early.\n\n";

    MqttClient pub(cfg.host, cfg.port, v, "sendonly-" + tag);
    pub.connect_mqtt();
    auto t0 = Clock::now();
    for (int i = 0; i < cfg.send_count; ++i) {
        const std::string payload = "seq=" + std::to_string(i);
        pub.publish(cfg.send_topic, bytes(payload));
        std::cout << "  published " << payload << "\n";
        if (cfg.send_delay_ms > 0) {
            sleep_ms(cfg.send_delay_ms);
        }
    }
    const double elapsed_s = std::chrono::duration<double>(Clock::now() - t0).count();
    pub.disconnect();

    MetricsSnap md = diff_metrics(fetch_metrics(cfg.host), m0);
    std::cout << "\n  done: " << cfg.send_count << " published in " << std::fixed
              << std::setprecision(2) << elapsed_s << " s";
    if (elapsed_s > 0) {
        std::cout << " (" << static_cast<int>(cfg.send_count / elapsed_s) << "/s)";
    }
    std::cout << "\n  broker health_delta: slow_consumer_disconnects="
              << md.slow_consumer_disconnects << " dropped_quota=" << md.dropped_quota
              << " pool_exhaustion=" << md.pool_exhaustion << " keepalive_disconnects="
              << md.keepalive_disconnects << "\n";
    return 0;
}

inline SoakResult bench_soak(const Config &cfg, int v, double duration_s)
{
    const std::string tag = "v" + std::to_string(v);
    MetricsSnap m0 = fetch_metrics(cfg.host);
    MqttClient sub(cfg.host, cfg.port, v, "soak-" + tag + "-sub");
    MqttClient pub(cfg.host, cfg.port, v, "soak-" + tag + "-pub");
    sub.connect_mqtt(120);
    pub.connect_mqtt(120);
    sub.subscribe("bench/" + tag + "/soak", 0);
    sub.set_recv_timeout_ms(500);

    struct State {
        std::atomic<int> received{0};
        double last_rx = 0;
    } state;
    std::atomic<bool> stop{false};

    std::thread reader([&] {
        auto last_ping = Clock::now();
        std::vector<uint8_t> buf;
        while (!stop.load()) {
            if (Clock::now() - last_ping > std::chrono::seconds(10)) {
                try {
                    sub.ping();
                } catch (...) {
                    return;
                }
                last_ping = Clock::now();
            }
            char chunk[65536];
            int n = ::recv(sub.native_socket(), chunk, sizeof(chunk), 0);
            if (n <= 0) {
                if (n < 0) {
                    continue;
                }
                return;
            }
            buf.insert(buf.end(), chunk, chunk + n);
            size_t consumed = 0;
            int c = count_publish_in_buffer(buf, consumed);
            if (c > 0) {
                state.received.fetch_add(c);
                state.last_rx =
                    std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
            }
            if (consumed > 0) {
                buf.erase(buf.begin(), buf.begin() + static_cast<long>(consumed));
            }
        }
    });

    const auto payload = bytes(R"({"bench":"soak","v":)" + std::to_string(v) + R"(,"pad":")" +
                               std::string(32, 'x') + "\"}");
    SoakResult sk;
    sk.version = ver_label(v);
    auto t0 = Clock::now();
    auto end = t0 + std::chrono::milliseconds(static_cast<int>(duration_s * 1000));
    auto next_mark = t0 + std::chrono::seconds(1);
    auto last_progress = t0;
    int published = 0;
    int last_pub = 0;
    int last_rx = 0;
    while (Clock::now() < end) {
        pub.publish("bench/" + tag + "/soak", payload);
        ++published;
        auto now = Clock::now();
        if (now - last_progress >= std::chrono::seconds(60)) {
            double elapsed = std::chrono::duration<double>(now - t0).count();
            std::cout << "    ... " << tag << " soak " << static_cast<int>(elapsed) << "s / "
                      << static_cast<int>(duration_s) << "s - " << published << " pub, "
                      << state.received.load() << " rx\n";
            last_progress = now;
        }
        if (now >= next_mark) {
            sk.per_second.push_back(
                {static_cast<int>(std::chrono::duration<double>(now - t0).count()), published - last_pub,
                 state.received.load() - last_rx});
            last_pub = published;
            last_rx = state.received.load();
            next_mark += std::chrono::seconds(1);
        }
    }
    sk.duration_s = std::chrono::duration<double>(Clock::now() - t0).count();
    sk.published = published;
    auto drain_deadline = Clock::now() + std::chrono::seconds(60);
    int seen = -1;
    while (Clock::now() < drain_deadline && state.received.load() < published) {
        int cur = state.received.load();
        if (cur != seen) {
            seen = cur;
            drain_deadline = Clock::now() + std::chrono::seconds(15);
        }
        sleep_ms(100);
    }
    stop.store(true);
    reader.join();
    sk.received = state.received.load();
    sk.publish_rate = sk.duration_s > 0 ? static_cast<int>(published / sk.duration_s) : 0;
    sk.delivery_pct = published > 0 ? (100.0 * sk.received / published) : 0;
    pub.disconnect();
    sub.disconnect();
    sk.health_delta = diff_metrics(fetch_metrics(cfg.host), m0);
    return sk;
}

inline std::string json_escape(const std::string &s)
{
    std::string o;
    for (char c : s) {
        if (c == '"') {
            o += "\\\"";
        } else if (c == '\\') {
            o += "\\\\";
        } else {
            o += c;
        }
    }
    return o;
}

inline void write_results(const Config &cfg, const std::string &started,
                          const std::vector<TestResult> &tests,
                          const std::vector<ThroughputResult> &tp,
                          const std::vector<LatencyResult> &lat,
                          const std::vector<SoakResult> &soaks)
{
    std::ofstream f(cfg.out);
    f << "{\n  \"started\": \"" << started << "\",\n  \"host\": \"" << cfg.host << "\",\n  \"tests\": [\n";
    for (size_t i = 0; i < tests.size(); ++i) {
        const auto &t = tests[i];
        f << "    {\"name\": \"" << json_escape(t.name) << "\", \"version\": \"" << t.version
          << "\", \"pass\": " << (t.pass ? "true" : "false") << ", \"detail\": \""
          << json_escape(t.detail) << "\", \"ms\": " << std::fixed << std::setprecision(1) << t.ms
          << "}";
        f << (i + 1 < tests.size() ? ",\n" : "\n");
    }
    f << "  ],\n  \"throughput\": [\n";
    for (size_t i = 0; i < tp.size(); ++i) {
        const auto &t = tp[i];
        f << "    {\"version\": \"" << t.version << "\", \"published\": " << t.published
          << ", \"received\": " << t.received << ", \"publish_rate\": " << t.publish_rate
          << ", \"e2e_rate\": " << t.e2e_rate << ", \"publish_s\": " << t.publish_s
          << ", \"e2e_s\": " << t.e2e_s << "}";
        f << (i + 1 < tp.size() ? ",\n" : "\n");
    }
    f << "  ],\n  \"latency\": [\n";
    for (size_t i = 0; i < lat.size(); ++i) {
        const auto &l = lat[i];
        f << "    {\"version\": \"" << l.version << "\", \"samples\": " << l.samples
          << ", \"p50_ms\": " << l.p50_ms << ", \"p95_ms\": " << l.p95_ms << ", \"p99_ms\": "
          << l.p99_ms << ", \"min_ms\": " << l.min_ms << ", \"max_ms\": " << l.max_ms
          << ", \"mean_ms\": " << l.mean_ms << ", \"raw_ms\": [";
        for (size_t j = 0; j < l.raw_ms.size(); ++j) {
            f << l.raw_ms[j] << (j + 1 < l.raw_ms.size() ? ", " : "");
        }
        f << "]}";
        f << (i + 1 < lat.size() ? ",\n" : "\n");
    }
    f << "  ],\n  \"soak\": [\n";
    for (size_t i = 0; i < soaks.size(); ++i) {
        const auto &s = soaks[i];
        f << "    {\"version\": \"" << s.version << "\", \"duration_s\": " << s.duration_s
          << ", \"published\": " << s.published << ", \"received\": " << s.received
          << ", \"publish_rate\": " << s.publish_rate << ", \"delivery_pct\": " << s.delivery_pct
          << ", \"per_second\": [\n";
        for (size_t j = 0; j < s.per_second.size(); ++j) {
            f << "        {\"t\": " << s.per_second[j][0] << ", \"pub\": " << s.per_second[j][1]
              << ", \"rx\": " << s.per_second[j][2] << "}";
            f << (j + 1 < s.per_second.size() ? ",\n" : "\n");
        }
        f << "      ], \"health_delta\": {"
          << "\"parser_errors\": " << s.health_delta.parser_errors
          << ", \"slow_consumer_disconnects\": " << s.health_delta.slow_consumer_disconnects
          << ", \"pool_exhaustion\": " << s.health_delta.pool_exhaustion
          << ", \"dropped_quota\": " << s.health_delta.dropped_quota
          << ", \"dropped_too_large\": " << s.health_delta.dropped_too_large
          << ", \"keepalive_disconnects\": " << s.health_delta.keepalive_disconnects << "}}";
        f << (i + 1 < soaks.size() ? ",\n" : "\n");
    }
    f << "  ]\n}\n";
}

inline Config parse_args(int argc, char **argv)
{
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char *flag) {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + flag);
            }
            return std::string(argv[++i]);
        };
        if (a == "--host") {
            cfg.host = need("--host");
        } else if (a == "--port") {
            cfg.port = static_cast<uint16_t>(std::stoi(need("--port")));
        } else if (a == "--out") {
            cfg.out = need("--out");
        } else if (a == "--bench-count") {
            cfg.bench_count = std::stoi(need("--bench-count"));
        } else if (a == "--lat-count") {
            cfg.lat_count = std::stoi(need("--lat-count"));
        } else if (a == "--soak-seconds") {
            cfg.soak_seconds = std::stod(need("--soak-seconds"));
        } else if (a == "--send-only") {
            cfg.send_only = true;
        } else if (a == "--send-topic") {
            cfg.send_topic = need("--send-topic");
        } else if (a == "--send-count") {
            cfg.send_count = std::stoi(need("--send-count"));
        } else if (a == "--send-delay-ms") {
            cfg.send_delay_ms = std::stoi(need("--send-delay-ms"));
        } else if (a == "--mqtt-version") {
            cfg.mqtt_version = std::stoi(need("--mqtt-version"));
        } else if (a == "--help" || a == "-h") {
            std::cout << "mqtt_device_suite [--host IP] [--port 1883] [--out results.json]\n"
                         "  [--bench-count N] [--lat-count N] [--soak-seconds N]\n"
                         "  [--send-only] [--send-topic TOPIC] [--send-count N]\n"
                         "  [--send-delay-ms N] [--mqtt-version 4|5]\n";
            std::exit(0);
        }
    }
    return cfg;
}

inline int run_suite(int argc, char **argv)
{
    if (!socket_init()) {
        std::cerr << "socket_init failed\n";
        return 2;
    }
    Config cfg = parse_args(argc, argv);

    if (cfg.send_only) {
        const int rc = run_send_only(cfg);
        socket_shutdown();
        return rc;
    }

    char started[32];
    std::time_t now = std::time(nullptr);
    std::strftime(started, sizeof(started), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

    std::vector<TestResult> tests;
    for (int v : {4, 5}) {
        std::cout << "=== MQTT " << (v == 4 ? "3.1.1" : "5") << " conformance ===\n";
        auto batch = make_tests(cfg, v);
        tests.insert(tests.end(), batch.begin(), batch.end());
    }
    std::cout << "=== Cross-version ===\n";
    auto cross = cross_version_tests(cfg);
    tests.insert(tests.end(), cross.begin(), cross.end());

    std::vector<ThroughputResult> tp;
    std::vector<LatencyResult> lat;
    std::cout << "=== Benchmarks ===\n";
    for (int v : {4, 5}) {
        auto tpr = bench_throughput(cfg, v);
        std::cout << "  throughput " << tpr.version << ": pub " << tpr.publish_rate << "/s, e2e "
                  << tpr.e2e_rate << "/s, delivered " << tpr.received << "/" << tpr.published
                  << "\n";
        tp.push_back(tpr);
        auto lr = bench_latency(cfg, v);
        std::cout << "  latency    " << lr.version << ": p50 " << lr.p50_ms << "ms p95 "
                  << lr.p95_ms << "ms p99 " << lr.p99_ms << "ms\n";
        lat.push_back(lr);
    }

    std::vector<SoakResult> soaks;
    if (cfg.soak_seconds > 0) {
        std::cout << "=== Soak (" << static_cast<int>(cfg.soak_seconds) << "s per version) ===\n";
        for (int v : {4, 5}) {
            auto sk = bench_soak(cfg, v, cfg.soak_seconds);
            std::cout << "  soak " << sk.version << ": " << sk.published << " published at "
                      << sk.publish_rate << "/s, delivered " << sk.received << " ("
                      << sk.delivery_pct << "%)\n";
            soaks.push_back(sk);
        }
    }

    int passed = 0;
    for (const auto &t : tests) {
        if (t.pass) {
            ++passed;
        }
    }
    std::cout << "\n" << passed << "/" << tests.size() << " conformance tests passed\n";
    write_results(cfg, started, tests, tp, lat, soaks);
    std::cout << "Results written to " << cfg.out << "\n";
    socket_shutdown();
    return passed == static_cast<int>(tests.size()) ? 0 : 1;
}

}  // namespace suite
}  // namespace device_suite
