// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#include "mqtt_broker/topic_intern.hpp"

#include <cassert>
#include <cstring>

using namespace mqtt_broker;

static void test_intern_dedup_and_release()
{
    TopicInternPool pool({8, 512, 256});
    TopicHandle a = pool.intern("sensors/temp");
    TopicHandle b = pool.intern("sensors/temp");
    assert(pool.valid(a));
    assert(a.slot == b.slot);
    assert(a.generation == b.generation);
    assert(pool.ref_count(a) == 2);

    pool.release(a);
    assert(pool.ref_count(b) == 1);

    pool.release(b);
    assert(!pool.valid(a));
    assert(pool.topic_count() == 0);
}

static void test_stale_handle_after_release()
{
    TopicInternPool pool({4, 256, 256});
    TopicHandle h = pool.intern("home/status");
    TopicHandle stale = h;
    pool.release(h);
    assert(!pool.valid(stale));
    assert(!pool.acquire(stale));
}

static void test_invalid_topic_rejected()
{
    TopicInternPool pool({4, 256, 256});
    assert(!pool.valid(pool.intern("")));
    assert(!pool.valid(pool.intern("/leading")));
    assert(!pool.valid(pool.intern(nullptr)));
}

int main()
{
    test_intern_dedup_and_release();
    test_stale_handle_after_release();
    test_invalid_topic_rejected();
    return 0;
}