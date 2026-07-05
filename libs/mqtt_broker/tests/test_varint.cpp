// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#include "mqtt_broker/varint.hpp"

#include <cassert>
#include <cstdint>

using namespace mqtt_broker;

static void check_roundtrip(uint32_t value)
{
    uint8_t buf[4];
    size_t n = varint_encode(value, buf, sizeof(buf));
    assert(n == varint_encoded_size(value));
    VarintDecode d = varint_decode(buf, n);
    assert(d.result == VarintResult::Ok);
    assert(d.value == value);
    assert(d.bytes_consumed == n);
}

static void test_boundaries()
{
    check_roundtrip(0);
    check_roundtrip(127);
    check_roundtrip(128);
    check_roundtrip(16383);
    check_roundtrip(16384);
    check_roundtrip(268435455u);

    uint8_t buf[4];
    varint_encode(268435455u, buf, sizeof(buf));

    VarintDecode partial = varint_decode(buf, 1);
    assert(partial.result == VarintResult::NeedMore);

    uint8_t overflow[] = {0xFF, 0xFF, 0xFF, 0xFF};
    VarintDecode bad = varint_decode(overflow, 4);
    assert(bad.result == VarintResult::TooLarge || bad.result == VarintResult::Overflow);
}

int main()
{
    test_boundaries();
    return 0;
}