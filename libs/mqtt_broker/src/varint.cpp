// varint.hpp implementation — Variable Byte Integer per MQTT 5 §1.5.5.

#include "mqtt_broker/varint.hpp"

namespace mqtt_broker {

static const uint32_t kVarintMax = 268435455u;  // §1.5.5 maximum representable value

VarintDecode varint_decode(const uint8_t *data, size_t len)
{
    VarintDecode out = {VarintResult::NeedMore, 0, 0};
    uint32_t value = 0;
    uint32_t multiplier = 1;

    for (size_t i = 0; i < len && i < 4; ++i) {
        uint8_t encoded = data[i];
        value += static_cast<uint32_t>(encoded & 0x7Fu) * multiplier;
        out.bytes_consumed = i + 1;

        if ((encoded & 0x80u) == 0) {  // Continuation bit clear — value complete
            out.result = VarintResult::Ok;
            out.value = value;
            return out;
        }

        if (multiplier > kVarintMax / 128u) {
            out.result = VarintResult::Overflow;
            return out;
        }
        multiplier *= 128u;
    }

    if (len >= 4) {
        out.result = VarintResult::TooLarge;  // Fifth continuation byte would be illegal
    }
    return out;
}

size_t varint_encode(uint32_t value, uint8_t *out, size_t out_cap)
{
    size_t i = 0;
    do {
        if (i >= out_cap) {
            return 0;
        }
        uint8_t byte = static_cast<uint8_t>(value % 128u);
        value /= 128u;
        if (value > 0) {
            byte = static_cast<uint8_t>(byte | 0x80u);
        }
        out[i++] = byte;
    } while (value > 0);
    return i;
}

size_t varint_encoded_size(uint32_t value)
{
    uint8_t buf[4];
    return varint_encode(value, buf, sizeof(buf));
}

}  // namespace mqtt_broker