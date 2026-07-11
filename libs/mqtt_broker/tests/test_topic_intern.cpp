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

static void test_intern_dedup_when_blob_full()
{
    // Blob is full of live topics; new topics fail, but dedup still works.
    TopicInternPool pool({4, 24, 256});
    TopicHandle keep = pool.intern("alpha/beta");
    assert(pool.valid(keep));

    TopicHandle b = pool.intern("012345678901");
    assert(pool.valid(b));
    assert(!pool.valid(pool.intern("012345678902")));  // 24-byte blob exhausted

    TopicHandle again = pool.intern("alpha/beta");
    assert(pool.valid(again));
    assert(again.slot == keep.slot);
    assert(again.generation == keep.generation);

    pool.release(b);
    pool.release(keep);
    pool.release(again);
}

static void test_blob_reclaimed_after_release()
{
    // Regression: release() must return blob bytes so intern/release churn on
    // transient topics never exhausts the arena (caused permanent TX death on
    // target after ~1800 publishes).
    TopicInternPool pool({4, 64, 256});
    for (int i = 0; i < 10000; ++i) {
        TopicHandle h = pool.intern("some/transient/topic/name");
        assert(pool.valid(h));
        pool.release(h);
    }
    assert(pool.bytes_used() == 0);
    assert(pool.topic_count() == 0);
}

static void test_compaction_preserves_live_topics()
{
    // Releasing a topic in the middle of the blob must not corrupt survivors.
    TopicInternPool pool({8, 128, 256});
    TopicHandle a = pool.intern("first/topic");
    TopicHandle b = pool.intern("second/topic");
    TopicHandle c = pool.intern("third/topic");
    assert(pool.valid(a) && pool.valid(b) && pool.valid(c));

    pool.release(b);  // hole in the middle -> compaction shifts c down

    assert(std::strcmp(pool.c_str(a), "first/topic") == 0);
    assert(std::strcmp(pool.c_str(c), "third/topic") == 0);

    // Dedup must still find the shifted survivors.
    TopicHandle c2 = pool.intern("third/topic");
    assert(c2.slot == c.slot && c2.generation == c.generation);
    pool.release(c2);

    // Freed bytes must be reusable.
    TopicHandle d = pool.intern("fourth/topic");
    assert(pool.valid(d));
    assert(std::strcmp(pool.c_str(d), "fourth/topic") == 0);

    pool.release(a);
    pool.release(c);
    pool.release(d);
    assert(pool.bytes_used() == 0);
}

int main()
{
    test_intern_dedup_and_release();
    test_stale_handle_after_release();
    test_invalid_topic_rejected();
    test_intern_dedup_when_blob_full();
    test_blob_reclaimed_after_release();
    test_compaction_preserves_live_topics();
    return 0;
}