// Minimal version-aware MQTT TCP client for live device conformance tests.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline int last_socket_error() { return WSAGetLastError(); }
inline void close_socket(socket_t s)
{
    if (s != kInvalidSocket) {
        closesocket(s);
    }
}
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
inline int last_socket_error() { return errno; }
inline void close_socket(socket_t s)
{
    if (s >= 0) {
        close(s);
    }
}
#endif

namespace device_suite {

inline std::vector<uint8_t> encode_varint(uint32_t n)
{
    std::vector<uint8_t> out;
    do {
        uint8_t b = static_cast<uint8_t>(n & 0x7F);
        n >>= 7;
        if (n) {
            b |= 0x80;
        }
        out.push_back(b);
    } while (n);
    return out;
}

inline bool socket_init()
{
#ifdef _WIN32
    WSADATA wsa{};
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

inline void socket_shutdown()
{
#ifdef _WIN32
    WSACleanup();
#endif
}

class MqttClient {
public:
    MqttClient(std::string host, uint16_t port, int version, std::string client_id)
        : host_(std::move(host)), port_(port), version_(version), client_id_(std::move(client_id))
    {
    }

    ~MqttClient() { close(); }

    MqttClient(const MqttClient &) = delete;
    MqttClient &operator=(const MqttClient &) = delete;

    std::vector<uint8_t> connect_mqtt(int keep_alive = 30, bool clean = true, uint32_t session_expiry = 0,
                                      const std::string *will_topic = nullptr,
                                      const std::vector<uint8_t> *will_payload = nullptr)
    {
        open_tcp();
        send_packet(0x10, build_connect(keep_alive, clean, session_expiry, will_topic, will_payload));
        auto ca = recv_packet(8000);
        if (ca.empty() || ca[0] != 0x20) {
            throw std::runtime_error("no CONNACK");
        }
        return ca;
    }

    std::vector<uint8_t> connect_raw(int keep_alive, bool clean, uint32_t session_expiry = 0,
                                     const std::string *will_topic = nullptr,
                                     const std::vector<uint8_t> *will_payload = nullptr)
    {
        open_tcp();
        send_packet(0x10, build_connect(keep_alive, clean, session_expiry, will_topic, will_payload));
        return recv_packet(8000);
    }

    void publish(const std::string &topic, const std::vector<uint8_t> &payload, uint8_t qos = 0,
                 uint16_t pid = 0, bool retain = false)
    {
        std::vector<uint8_t> body;
        append_u16(body, static_cast<uint16_t>(topic.size()));
        body.insert(body.end(), topic.begin(), topic.end());
        if (qos > 0) {
            append_u16(body, pid);
        }
        if (version_ == 5) {
            body.push_back(0x00);
        }
        body.insert(body.end(), payload.begin(), payload.end());
        uint8_t fh = 0x30 | static_cast<uint8_t>((qos << 1) | (retain ? 1 : 0));
        send_packet(fh, body);
    }

    void subscribe(const std::string &topic, uint8_t qos = 0, uint16_t pid = 1)
    {
        std::vector<uint8_t> body;
        append_u16(body, pid);
        if (version_ == 5) {
            body.push_back(0x00);
        }
        append_u16(body, static_cast<uint16_t>(topic.size()));
        body.insert(body.end(), topic.begin(), topic.end());
        body.push_back(qos);
        send_packet(0x82, body);
        auto ack = recv_packet();
        if (ack.empty() || ack[0] != 0x90) {
            throw std::runtime_error("SUBACK failed");
        }
    }

    std::vector<uint8_t> unsubscribe(const std::string &topic, uint16_t pid = 1)
    {
        std::vector<uint8_t> body;
        append_u16(body, pid);
        if (version_ == 5) {
            body.push_back(0x00);
        }
        append_u16(body, static_cast<uint16_t>(topic.size()));
        body.insert(body.end(), topic.begin(), topic.end());
        send_packet(0xA2, body);
        return recv_packet();
    }

    void ping()
    {
        const std::vector<uint8_t> pkt{0xC0, 0x00};
        send_all(pkt);
    }

    std::vector<uint8_t> ping_recv()
    {
        ping();
        return recv_packet();
    }

    void send_ack(uint8_t ptype, uint16_t pid)
    {
        uint8_t fh = static_cast<uint8_t>((ptype << 4) | (ptype == 6 ? 0x02 : 0x00));
        const std::vector<uint8_t> pkt = {fh, 0x02, static_cast<uint8_t>(pid >> 8),
                                            static_cast<uint8_t>(pid & 0xFF)};
        send_all(pkt);
    }

    struct PublishMsg {
        std::string topic;
        std::vector<uint8_t> payload;
        uint8_t qos = 0;
        bool retain = false;
        uint16_t pid = 0;
    };

    PublishMsg recv_publish(int timeout_ms = 5000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            int remain = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              deadline - std::chrono::steady_clock::now())
                                              .count());
            if (remain < 1) {
                remain = 1;
            }
            auto pkt = recv_packet(remain);
            if (pkt.empty()) {
                throw std::runtime_error("connection closed awaiting PUBLISH");
            }
            uint8_t ptype = pkt[0] >> 4;
            if (ptype != 3) {
                if (ptype == 6) {
                    uint16_t pid = static_cast<uint16_t>((pkt[2] << 8) | pkt[3]);
                    send_ack(7, pid);
                }
                continue;
            }
            return parse_publish(pkt);
        }
        throw std::runtime_error("no PUBLISH within timeout");
    }

    std::vector<uint8_t> recv_packet(int timeout_ms = 5000)
    {
        auto fh = recv_exact(1, timeout_ms);
        if (fh.empty()) {
            return {};
        }
        uint32_t rl = 0;
        uint32_t mul = 1;
        while (true) {
            auto b = recv_exact(1, timeout_ms);
            if (b.empty()) {
                return {};
            }
            rl += static_cast<uint32_t>(b[0] & 0x7F) * mul;
            mul *= 128;
            if (!(b[0] & 0x80)) {
                break;
            }
        }
        std::vector<uint8_t> body;
        if (rl > 0) {
            body = recv_exact(static_cast<int>(rl), timeout_ms);
            if (body.size() != rl) {
                return {};
            }
        }
        std::vector<uint8_t> out;
        out.push_back(fh[0]);
        out.push_back(static_cast<uint8_t>(rl & 0xFF));  // simplified storage
        out.insert(out.end(), body.begin(), body.end());
        // Reconstruct full packet for tests that compare hex
        out.clear();
        out.push_back(fh[0]);
        auto rl_enc = encode_varint(rl);
        out.insert(out.end(), rl_enc.begin(), rl_enc.end());
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }

    void disconnect()
    {
        try {
            const std::vector<uint8_t> pkt{0xE0, 0x00};
            send_all(pkt);
        } catch (...) {
        }
        close();
    }

    void close()
    {
        if (sock_ != kInvalidSocket) {
            close_socket(sock_);
            sock_ = kInvalidSocket;
        }
    }

    void abrupt_close() { close(); }

    socket_t native_socket() const { return sock_; }

    void set_recv_timeout_ms(int ms)
    {
#ifdef _WIN32
        DWORD tv = static_cast<DWORD>(ms);
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&tv), sizeof(tv));
#else
        timeval tv{};
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

private:
    std::string host_;
    uint16_t port_;
    int version_;
    std::string client_id_;
    socket_t sock_ = kInvalidSocket;

    static void append_u16(std::vector<uint8_t> &b, uint16_t v)
    {
        b.push_back(static_cast<uint8_t>(v >> 8));
        b.push_back(static_cast<uint8_t>(v & 0xFF));
    }

    void open_tcp()
    {
        close();
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *res = nullptr;
        const std::string port_str = std::to_string(port_);
        if (getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
            throw std::runtime_error("getaddrinfo failed");
        }
        sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock_ == kInvalidSocket) {
            freeaddrinfo(res);
            throw std::runtime_error("socket() failed");
        }
        int yes = 1;
        setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&yes), sizeof(yes));
        if (::connect(sock_, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
            freeaddrinfo(res);
            close();
            throw std::runtime_error("connect failed");
        }
        freeaddrinfo(res);
        set_recv_timeout_ms(8000);
    }

    std::vector<uint8_t> build_connect(int keep_alive, bool clean, uint32_t session_expiry,
                                       const std::string *will_topic,
                                       const std::vector<uint8_t> *will_payload)
    {
        std::vector<uint8_t> body{'\x00', '\x04', 'M', 'Q', 'T', 'T'};
        body.push_back(static_cast<uint8_t>(version_));
        uint8_t flags = clean ? 0x02 : 0x00;
        if (will_topic) {
            flags |= 0x04;
        }
        body.push_back(flags);
        append_u16(body, static_cast<uint16_t>(keep_alive));
        if (version_ == 5) {
            std::vector<uint8_t> props;
            if (session_expiry) {
                props.push_back(0x11);
                props.push_back(static_cast<uint8_t>(session_expiry >> 24));
                props.push_back(static_cast<uint8_t>(session_expiry >> 16));
                props.push_back(static_cast<uint8_t>(session_expiry >> 8));
                props.push_back(static_cast<uint8_t>(session_expiry));
            }
            auto plen = encode_varint(static_cast<uint32_t>(props.size()));
            body.insert(body.end(), plen.begin(), plen.end());
            body.insert(body.end(), props.begin(), props.end());
        }
        append_u16(body, static_cast<uint16_t>(client_id_.size()));
        body.insert(body.end(), client_id_.begin(), client_id_.end());
        if (will_topic && will_payload) {
            if (version_ == 5) {
                body.push_back(0x00);
            }
            append_u16(body, static_cast<uint16_t>(will_topic->size()));
            body.insert(body.end(), will_topic->begin(), will_topic->end());
            append_u16(body, static_cast<uint16_t>(will_payload->size()));
            body.insert(body.end(), will_payload->begin(), will_payload->end());
        }
        return body;
    }

    void send_packet(uint8_t fh, const std::vector<uint8_t> &body)
    {
        std::vector<uint8_t> pkt{fh};
        auto rl = encode_varint(static_cast<uint32_t>(body.size()));
        pkt.insert(pkt.end(), rl.begin(), rl.end());
        pkt.insert(pkt.end(), body.begin(), body.end());
        send_all(pkt);
    }

    void send_all(const uint8_t *data, size_t len)
    {
        size_t off = 0;
        while (off < len) {
            int n = ::send(sock_, reinterpret_cast<const char *>(data + off),
                           static_cast<int>(len - off), 0);
            if (n <= 0) {
                throw std::runtime_error("send failed");
            }
            off += static_cast<size_t>(n);
        }
    }

    void send_all(const std::vector<uint8_t> &data) { send_all(data.data(), data.size()); }

    std::vector<uint8_t> recv_exact(int n, int timeout_ms)
    {
        set_recv_timeout_ms(timeout_ms);
        std::vector<uint8_t> buf(static_cast<size_t>(n));
        size_t got = 0;
        while (got < static_cast<size_t>(n)) {
            int r = ::recv(sock_, reinterpret_cast<char *>(buf.data() + got),
                           static_cast<int>(n - static_cast<int>(got)), 0);
            if (r <= 0) {
                return {};
            }
            got += static_cast<size_t>(r);
        }
        return buf;
    }

    PublishMsg parse_publish(const std::vector<uint8_t> &pkt)
    {
        PublishMsg m;
        m.qos = static_cast<uint8_t>((pkt[0] >> 1) & 0x03);
        m.retain = (pkt[0] & 0x01) != 0;
        size_t i = 1;
        while (i < pkt.size() && (pkt[i] & 0x80)) {
            ++i;
        }
        ++i;
        uint16_t tlen = static_cast<uint16_t>((pkt[i] << 8) | pkt[i + 1]);
        i += 2;
        m.topic.assign(reinterpret_cast<const char *>(&pkt[i]), tlen);
        i += tlen;
        if (m.qos > 0) {
            m.pid = static_cast<uint16_t>((pkt[i] << 8) | pkt[i + 1]);
            i += 2;
        }
        if (version_ == 5) {
            uint8_t plen = pkt[i++];
            i += plen;
        }
        m.payload.assign(pkt.begin() + static_cast<long>(i), pkt.end());
        if (m.qos == 1) {
            send_ack(4, m.pid);
        } else if (m.qos == 2) {
            send_ack(5, m.pid);
        }
        return m;
    }
};

}  // namespace device_suite
