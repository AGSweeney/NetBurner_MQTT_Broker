#include "mqtt_broker/message_pool.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

using namespace mqtt_broker;

static void test_ref_generation()
{
    MessagePool pool({512, 16});
    MessageHandle h = pool.acquire_empty();
    assert(message_handle_valid(h));
    assert(pool.msg_ref_count(h) == 1);

    assert(pool.msg_acquire(h));
    assert(pool.msg_ref_count(h) == 2);

    pool.msg_release(h);
    assert(pool.msg_ref_count(h) == 1);

    MessageHandle stale = h;
    pool.msg_release(h);
    assert(!pool.msg_valid(stale));
    assert(!pool.msg_acquire(stale));
}

static void test_byte_at_a_time_append()
{
    MessagePool pool({512, 8});
    MessageHandle h = pool.acquire_empty();
    assert(message_handle_valid(h));

    for (int i = 0; i < 199; ++i) {
        uint8_t b = static_cast<uint8_t>('x');
        assert(pool.append_payload(h, &b, 1));
    }
    assert(pool.payload_size(h) == 199);
    assert(pool.blocks_allocated() == 1);

    pool.msg_release(h);
    assert(pool.blocks_allocated() == 0);
}

static void test_fanout_eight_kb_six_refs()
{
    MessagePool pool({512, 32});
    MessageHandle h = pool.acquire_empty();
    assert(message_handle_valid(h));

    std::vector<uint8_t> chunk(512, 0xAB);
    for (int i = 0; i < 16; ++i) {
        assert(pool.append_block(h, chunk.data(), chunk.size()));
    }

    std::vector<MessageHandle> refs;
    for (int i = 0; i < 6; ++i) {
        assert(pool.msg_acquire(h));
        refs.push_back(h);
    }

    pool.msg_release(h);
    for (const auto &r : refs) {
        pool.msg_release(r);
    }

    assert(pool.blocks_allocated() == 0);
    assert(pool.blocks_free() == 32);
}

int main()
{
    test_ref_generation();
    test_byte_at_a_time_append();
    test_fanout_eight_kb_six_refs();
    return 0;
}