#include "mqtt_broker/encode.hpp"
#include "mqtt_broker/property_pool.hpp"
#include "mqtt_broker/property_rules.hpp"

#include <cassert>
#include <cstdint>

using namespace mqtt_broker;

static void test_subscription_identifier_rejected()
{
    PropertyPool pool({4, 16, 2048});
    const uint8_t props[] = {0x02, SubscriptionIdentifier, 0x01};
    PropertyHandle h = NULL_PROPERTY;
    PropertyValidationResult vr =
        parse_and_validate_properties(PacketType::Subscribe, props, sizeof(props), &pool, &h);
    assert(!vr.ok);
    assert(vr.reason == ReasonCode::SubscriptionIdentifiersNotSupported);
}

static void test_publish_user_property_ok()
{
    PropertyPool pool({4, 16, 2048});
    PropertyHandle h = pool.acquire();
    assert(pool.valid(h));
    assert(pool.add_user_property(h, "k", 1, "v", 1));

    PropertyValidationResult vr = validate_properties(PacketType::Publish, h, pool);
    assert(vr.ok);
    pool.release(h);
}

static void test_duplicate_session_expiry_rejected()
{
    PropertyPool pool({4, 16, 2048});
    const uint8_t props[] = {
        0x0A, SessionExpiryInterval, 0x00, 0x00, 0x00, 0x0A,
        SessionExpiryInterval, 0x00, 0x00, 0x00, 0x14,
    };
    PropertyHandle h = NULL_PROPERTY;
    PropertyValidationResult vr =
        parse_and_validate_properties(PacketType::Connect, props, sizeof(props), &pool, &h);
    assert(!vr.ok);
    assert(vr.reason == ReasonCode::ProtocolError);
}

static void test_utf8_validation()
{
    const uint8_t bad[] = {0xFF, 0x01};
    assert(!utf8_valid(bad, sizeof(bad)));
    const uint8_t good[] = {'h', 'i'};
    assert(utf8_valid(good, sizeof(good)));
}

static void test_serialize_forward_properties()
{
    PropertyPool pool({8, 32, 2048});
    PropertyHandle h = pool.acquire();
    assert(pool.valid(h));
    assert(pool.add_byte(h, PayloadFormatIndicator, 0x01));
    assert(pool.add_utf8(h, ContentType, "text/plain", 10));
    assert(pool.add_u32(h, MessageExpiryInterval, 60));

    uint8_t out[128];
    uint32_t expiry = 0;
    size_t len = serialize_forward_properties(pool, h, out, sizeof(out), &expiry);
    assert(expiry == 60);
    assert(len > 0);
    assert(out[0] == PayloadFormatIndicator);
    assert(out[1] == 0x01);
    // Message Expiry Interval must not appear in the serialized blob.
    for (size_t i = 0; i + 1 < len; ++i) {
        assert(out[i] != MessageExpiryInterval);
    }
    pool.release(h);
}

int main()
{
    test_subscription_identifier_rejected();
    test_publish_user_property_ok();
    test_duplicate_session_expiry_rejected();
    test_utf8_validation();
    test_serialize_forward_properties();
    return 0;
}