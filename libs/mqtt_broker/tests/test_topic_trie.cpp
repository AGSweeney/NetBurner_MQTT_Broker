// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#include "mqtt_broker/topic_trie.hpp"

#include <cassert>
#include <cstring>

using namespace mqtt_broker;

static void test_wildcard_match()
{
    assert(TopicTrie::topic_matches_filter("sensors/a", "sensors/+"));
    assert(TopicTrie::topic_matches_filter("sensors/a/b", "sensors/#"));
    assert(TopicTrie::topic_matches_filter("other", "#"));
    assert(!TopicTrie::topic_matches_filter("sensors/a", "home/+"));
}

static void test_subscribe_and_match()
{
    TopicTrie trie({32, 16});
    uint16_t sub_id = 0;
    assert(trie.subscribe("sensors/#", 1, 1, 0x00, &sub_id));
    assert(sub_id != 0);

    TopicSubscription out[4];
    size_t n = trie.match("sensors/temp", out, 4);
    assert(n == 1);
    assert(out[0].session_id == 1);
    assert(out[0].max_qos == 1);

    assert(trie.unsubscribe("sensors/#", 1));
    n = trie.match("sensors/temp", out, 4);
    assert(n == 0);
}

static void test_overlap_single_delivery()
{
    TopicTrie trie({32, 16});
    assert(trie.subscribe("sensors/#", 1, 0, 0x00, nullptr));
    assert(trie.subscribe("sensors/+", 1, 1, 0x00, nullptr));

    TopicSubscription out[4];
    size_t n = trie.match("sensors/a", out, 4);
    assert(n == 2);
}

int main()
{
    test_wildcard_match();
    test_subscribe_and_match();
    test_overlap_single_delivery();
    return 0;
}