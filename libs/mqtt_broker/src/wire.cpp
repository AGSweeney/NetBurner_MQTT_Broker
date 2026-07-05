// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// wire.hpp implementation — big-endian scalars and UTF-8 Encoded Strings (§1.5).

#include "mqtt_broker/wire.hpp"

#include <cstring>

namespace mqtt_broker {

uint16_t read_u16_be(const uint8_t *p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t read_u32_be(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

void write_u16_be(uint8_t *p, uint16_t v)
{
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[1] = static_cast<uint8_t>(v & 0xFFu);
}

void write_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[3] = static_cast<uint8_t>(v & 0xFFu);
}

size_t encode_utf8_string(const char *str, uint8_t *out, size_t out_cap)
{
    // §1.5.4: Two Byte integer length + UTF-8 payload; max string length 65535.
    if (out_cap < 2 || str == nullptr) {
        return 0;
    }
    size_t slen = std::strlen(str);
    if (slen > 0xFFFFu || out_cap < 2 + slen) {
        return 0;
    }
    write_u16_be(out, static_cast<uint16_t>(slen));
    if (slen > 0) {
        std::memcpy(out + 2, str, slen);
    }
    return 2 + slen;
}

size_t decode_utf8_string(const uint8_t *in, size_t in_len, char *out, size_t out_cap)
{
    // Returns total wire bytes consumed (2 + slen) on success, 0 on truncation.
    if (in_len < 2 || out_cap == 0) {
        return 0;
    }
    uint16_t slen = read_u16_be(in);
    if (static_cast<size_t>(slen) + 2 > in_len) {
        return 0;
    }
    size_t copy = slen;
    if (copy >= out_cap) {
        copy = out_cap - 1;
    }
    if (copy > 0) {
        std::memcpy(out, in + 2, copy);
    }
    out[copy] = '\0';
    return 2 + slen;
}

#if defined(MQTT_BROKER_HOST_LE)
void wire_self_test()
{
    uint8_t buf[4];
    write_u16_be(buf, 0x1234);
    if (read_u16_be(buf) != 0x1234) {
        return;
    }
    write_u32_be(buf, 0xAABBCCDDu);
    if (read_u32_be(buf) != 0xAABBCCDDu) {
        return;
    }
}
#endif

}  // namespace mqtt_broker