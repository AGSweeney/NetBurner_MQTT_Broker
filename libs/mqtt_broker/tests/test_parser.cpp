#include "mqtt_broker/message_pool.hpp"
#include "mqtt_broker/mqtt_types.hpp"
#include "mqtt_broker/parser.hpp"
#include "mqtt_broker/property_pool.hpp"
#include "mqtt_broker/broker_limits.hpp"
#include "mqtt_broker/varint.hpp"
#include "mqtt_broker/wire.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace mqtt_broker;

static size_t feed_all(PacketParser &parser, const std::vector<uint8_t> &pkt)
{
    size_t total = 0;
    for (size_t i = 0; i < pkt.size(); ++i) {
        total += parser.feed(&pkt[i], 1);
    }
    return total;
}

static std::vector<uint8_t> build_pingreq()
{
    return {0xC0, 0x00};
}

static std::vector<uint8_t> build_publish_qos0(const char *topic, const uint8_t *payload,
                                               size_t payload_len)
{
    std::vector<uint8_t> body;
    uint8_t topic_enc[300];
    size_t tlen = encode_utf8_string(topic, topic_enc, sizeof(topic_enc));
    body.insert(body.end(), topic_enc, topic_enc + tlen);
    body.push_back(0x00);

    uint32_t remaining = static_cast<uint32_t>(body.size() + payload_len);
    uint8_t rl_buf[4];
    size_t rl_size = varint_encode(remaining, rl_buf, sizeof(rl_buf));

    std::vector<uint8_t> pkt;
    pkt.push_back(0x30);
    pkt.insert(pkt.end(), rl_buf, rl_buf + rl_size);
    pkt.insert(pkt.end(), body.begin(), body.end());
    if (payload_len > 0 && payload != nullptr) {
        pkt.insert(pkt.end(), payload, payload + payload_len);
    }
    return pkt;
}

static std::vector<uint8_t> build_connect_max_packet(const char *client_id, uint32_t max_pkt)
{
    std::vector<uint8_t> body;
    const char proto[] = {'M', 'Q', 'T', 'T'};
    body.insert(body.end(), {0x00, 0x04});
    body.insert(body.end(), proto, proto + 4);
    body.insert(body.end(), {0x05, 0x02, 0x00, 0x3c});
    body.push_back(0x05);
    body.push_back(MaximumPacketSize);
    body.push_back(static_cast<uint8_t>((max_pkt >> 24) & 0xFFu));
    body.push_back(static_cast<uint8_t>((max_pkt >> 16) & 0xFFu));
    body.push_back(static_cast<uint8_t>((max_pkt >> 8) & 0xFFu));
    body.push_back(static_cast<uint8_t>(max_pkt & 0xFFu));
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

static void test_connect_max_packet_property()
{
    MessagePool msg_pool({512, 8});
    PropertyPool prop_pool({4, 16, 2048});
    PacketParser parser({16384, 256, 1024}, &msg_pool, &prop_pool);
    auto pkt = build_connect_max_packet("limited", 32);
    feed_all(parser, pkt);
    assert(parser.packet_ready());
    assert(parser.packet().client_max_packet_size == 32);
}

static void test_pingreq()
{
    MessagePool msg_pool({512, 8});
    PropertyPool prop_pool({4, 16, 2048});
    PacketParser parser({16384, 256, 1024}, &msg_pool, &prop_pool);

    auto pkt = build_pingreq();
    assert(feed_all(parser, pkt) == pkt.size());
    assert(parser.packet_ready());
    assert(parser.packet().type == PacketType::Pingreq);
}

static void test_publish_fragmented_one_byte()
{
    MessagePool msg_pool({512, 8});
    PropertyPool prop_pool({4, 16, 2048});
    PacketParser parser({16384, 256, 1024}, &msg_pool, &prop_pool);

    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    auto pkt = build_publish_qos0("sensors/a", payload, sizeof(payload));
    assert(feed_all(parser, pkt) == pkt.size());
    assert(parser.packet_ready());
    assert(parser.packet().type == PacketType::Publish);
    assert(std::strcmp(parser.packet().topic, "sensors/a") == 0);
    assert(parser.packet().payload_len == sizeof(payload));
}

static void test_subscribe_subscription_id_rejected()
{
    MessagePool msg_pool({512, 8});
    PropertyPool prop_pool({4, 16, 2048});
    PacketParser parser({16384, 256, 1024}, &msg_pool, &prop_pool);

    std::vector<uint8_t> pkt = {
        0x82, 0x09, 0x00, 0x01, 0x02, SubscriptionIdentifier, 0x01,
        0x00, 0x01, 'a', 0x00,
    };

    feed_all(parser, pkt);
    assert(parser.phase() == ParsePhase::Error);
    assert(parser.error_reason() == ReasonCode::SubscriptionIdentifiersNotSupported);
}

static void test_multiple_packets_per_feed()
{
    MessagePool msg_pool({512, 8});
    PropertyPool prop_pool({4, 16, 2048});
    PacketParser parser({16384, 256, 1024}, &msg_pool, &prop_pool);

    auto pkt = build_pingreq();
    size_t n = parser.feed(pkt.data(), pkt.size());
    assert(n == pkt.size());
    assert(parser.packet_ready());
    parser.reset();
    assert(parser.phase() == ParsePhase::Idle);
}

static void broker_maximum_packet_size_limits_inbound()
{
    MessagePool msg_pool({512, 8});
    PropertyPool prop_pool({4, 16, 2048});
    PacketParser parser({BrokerLimits::MaxPacketBytes, 256, 1024}, &msg_pool, &prop_pool);

    const uint32_t oversize = static_cast<uint32_t>(BrokerLimits::MaxPacketBytes) + 1u;
    uint8_t rl[4];
    size_t rl_size = varint_encode(oversize, rl, sizeof(rl));
    std::vector<uint8_t> pkt;
    pkt.push_back(0xC0);  // PINGREQ type with oversize remaining length
    pkt.insert(pkt.end(), rl, rl + rl_size);

    feed_all(parser, pkt);
    assert(parser.phase() == ParsePhase::Error);
    assert(parser.error_reason() == ReasonCode::PacketTooLarge);
    printf("PASS broker_maximum_packet_size_limits_inbound\n");
}

int main()
{
    test_connect_max_packet_property();
    broker_maximum_packet_size_limits_inbound();
    test_pingreq();
    test_publish_fragmented_one_byte();
    test_subscribe_subscription_id_rejected();
    test_multiple_packets_per_feed();
    return 0;
}