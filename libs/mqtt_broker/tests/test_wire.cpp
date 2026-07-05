// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#include "mqtt_broker/encode.hpp"
#include "mqtt_broker/varint.hpp"
#include "mqtt_broker/wire.hpp"

#include <cassert>
#include <cstring>

using namespace mqtt_broker;

static void test_connack_remaining_length()
{
    uint8_t buf[256];
    size_t n = encode_connack(buf, sizeof(buf), ReasonCode::Success, false, nullptr);
    assert(n >= 4);
    assert(buf[0] == 0x20);

    VarintDecode rl = varint_decode(buf + 1, n - 1);
    assert(rl.result == VarintResult::Ok);
    assert(1 + rl.bytes_consumed + rl.value == n);
}

static void test_u16_roundtrip()
{
    uint8_t buf[2];
    write_u16_be(buf, 0);
    assert(read_u16_be(buf) == 0);
    write_u16_be(buf, 0xFFFF);
    assert(read_u16_be(buf) == 0xFFFF);
    write_u16_be(buf, 0x1234);
    assert(read_u16_be(buf) == 0x1234);
}

static void test_u32_roundtrip()
{
    uint8_t buf[4];
    write_u32_be(buf, 0xAABBCCDDu);
    assert(read_u32_be(buf) == 0xAABBCCDDu);
}

static void test_utf8_string()
{
    uint8_t enc[32];
    size_t n = encode_utf8_string("sensors/a", enc, sizeof(enc));
    assert(n == 11);
    assert(enc[0] == 0 && enc[1] == 9);

    char out[16];
    size_t d = decode_utf8_string(enc, n, out, sizeof(out));
    assert(d == 11);
    assert(std::strcmp(out, "sensors/a") == 0);
}

int main()
{
    test_connack_remaining_length();
    test_u16_roundtrip();
    test_u32_roundtrip();
    test_utf8_string();
#if defined(MQTT_BROKER_HOST_LE)
    wire_self_test();
#endif
    return 0;
}