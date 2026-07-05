#include "mqtt_broker/message_pool.hpp"
#include "mqtt_broker/parser.hpp"
#include "mqtt_broker/property_pool.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <vector>

using namespace mqtt_broker;

static uint32_t fuzz_rng = 0x12345678u;

static uint32_t rng_next()
{
    fuzz_rng = fuzz_rng * 1664525u + 1013904223u;
    return fuzz_rng;
}

static uint8_t rng_byte()
{
    return static_cast<uint8_t>(rng_next() & 0xFFu);
}

static std::vector<uint8_t> corpus_pingreq()
{
    return {0xC0, 0x00};
}

static std::vector<uint8_t> corpus_publish_minimal()
{
    return {0x30, 0x0B, 0x00, 0x01, 'a', 0x00, 'p', 'a', 'y', 'l', 'o', 'a', 'd'};
}

static std::vector<uint8_t> corpus_remaining_length_boundary()
{
    return {0xC0, 0xFF, 0xFF, 0xFF, 0x7F};
}

static std::vector<uint8_t> mutate(const std::vector<uint8_t> &base)
{
    std::vector<uint8_t> out = base;
    if (out.empty()) {
        out.push_back(rng_byte());
        return out;
    }
    size_t op = rng_next() % 4;
    if (op == 0) {
        out[rng_next() % out.size()] = rng_byte();
    } else if (op == 1 && out.size() < 256) {
        out.insert(out.begin() + (rng_next() % (out.size() + 1)), rng_byte());
    } else if (op == 2 && out.size() > 1) {
        out.erase(out.begin() + (rng_next() % out.size()));
    } else if (out.size() < 512) {
        size_t extra = 1 + (rng_next() % 8);
        for (size_t i = 0; i < extra; ++i) {
            out.push_back(rng_byte());
        }
    }
    return out;
}

static void run_parser(const std::vector<uint8_t> &pkt, MessagePool &msg_pool,
                       PropertyPool &prop_pool)
{
    PacketParser parser({16384, 256, 1024}, &msg_pool, &prop_pool);
    size_t off = 0;
    while (off < pkt.size()) {
        size_t chunk = 1 + (rng_next() % 4);
        if (off + chunk > pkt.size()) {
            chunk = pkt.size() - off;
        }
        parser.feed(pkt.data() + off, chunk);
        off += chunk;

        if (parser.packet_ready() || parser.phase() == ParsePhase::Error) {
            if (parser.packet_ready() && msg_pool.msg_valid(parser.packet().payload)) {
                msg_pool.msg_release(parser.packet().payload);
            }
            parser.reset();
        }
    }

    assert(msg_pool.blocks_allocated() + msg_pool.blocks_free() == 32);
}

int main()
{
    MessagePool msg_pool({512, 32});
    PropertyPool prop_pool({8, 16, 4096});

    std::vector<std::vector<uint8_t>> seeds = {
        corpus_pingreq(),
        corpus_publish_minimal(),
        corpus_remaining_length_boundary(),
        {},
        {0x10},
        {0x30, 0x00},
        {0x82, 0x05, 0x00, 0x01, 0x00, 0x00, 0x01, 'x', 0x00},
    };

    const int iterations = 1000000;
    for (int i = 0; i < iterations; ++i) {
        const auto &seed = seeds[static_cast<size_t>(rng_next() % seeds.size())];
        std::vector<uint8_t> pkt = (rng_next() % 3) == 0 ? mutate(seed) : seed;
        run_parser(pkt, msg_pool, prop_pool);
    }

    return 0;
}