// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef MQTT_BROKER_TOPIC_TRIE_HPP
#define MQTT_BROKER_TOPIC_TRIE_HPP

// Topic filter trie for subscription storage and PUBLISH routing.
// Filters are stored as segment paths with dedicated +/# wildcard nodes;
// match() walks the trie against an incoming topic and returns every
// subscription whose filter matches. Validation follows MQTT-5.0 §4.7.1–§4.7.3.

#include <cstddef>
#include <cstdint>

namespace mqtt_broker {

struct TopicTrieConfig {
    size_t max_nodes;           // trie node pool capacity
    size_t max_subscriptions;   // flat subscription slot pool capacity
};

// One matching subscription returned by match(); options_byte is the raw
// SUBSCRIBE Subscription Options octet (see subscription_options.hpp).
struct TopicSubscription {
    uint16_t sub_id;        // broker-assigned id for this subscription slot
    uint16_t session_id;    // owning session slot
    uint8_t max_qos;        // granted maximum QoS for delivery
    uint8_t options_byte;   // no_local / retain_as_published / retain_handling
};

class TopicTrie {
public:
    explicit TopicTrie(const TopicTrieConfig &cfg);
    ~TopicTrie();

    TopicTrie(const TopicTrie &) = delete;
    TopicTrie &operator=(const TopicTrie &) = delete;

    // Inserts or updates filter for session_id. On duplicate filter for the
    // same session, updates max_qos/options and sets *out_existed. Returns false
    // when filter/options are invalid or pools are full. *out_sub_id always set
    // on success.
    bool subscribe(const char *filter, uint16_t session_id, uint8_t max_qos, uint8_t options_byte,
                   uint16_t *out_sub_id, bool *out_existed = nullptr);
    // Removes one filter for session_id. Returns false if no such subscription.
    bool unsubscribe(const char *filter, uint16_t session_id);
    // Removes every subscription owned by session_id (session teardown).
    void unsubscribe_all(uint16_t session_id);
    // Collects up to out_cap matches for topic into out; returns count written.
    size_t match(const char *topic, TopicSubscription *out, size_t out_cap) const;
    size_t subscription_count() const;

    // MQTT-5.0 §4.7.1 — topic names must not contain + or #.
    static bool topic_valid(const char *topic);
    // MQTT-5.0 §4.7.1–§4.7.2 — wildcard placement rules for filters.
    static bool filter_valid(const char *filter);
    // MQTT-5.0 §4.7.3 — segment-wise topic/filter matching (also used at match time).
    static bool topic_matches_filter(const char *topic, const char *filter);

private:
    struct Node {
        char segment[256];
        uint16_t first_child;     // 0xFFFF = none
        uint16_t next_sibling;    // sibling chain under same parent
        uint16_t first_sub;       // head of SubEntry chain at this trie node
        bool terminal;            // a filter ends at this node
        bool is_wildcard_plus;    // segment is '+'
        bool is_wildcard_hash;    // segment is '#'
    };

    struct SubEntry {
        uint16_t sub_id;
        uint16_t session_id;
        uint8_t max_qos;
        uint8_t options_byte;
        uint16_t node_id;         // trie node where this sub is registered
        uint16_t next;            // next SubEntry on same node
        char filter[256];         // full filter string for duplicate detection
        bool in_use;
    };

    uint16_t alloc_node();
    uint16_t find_or_add_child(uint16_t parent, const char *segment, bool plus, bool hash);
    void unlink_sub(uint16_t slot);
    void collect_matches(uint16_t node, const char *topic, const char *rest,
                         TopicSubscription *out, size_t out_cap, size_t *count) const;

    Node *nodes_;
    size_t node_count_;
    size_t node_cap_;
    uint16_t root_;
    SubEntry *subs_;
    size_t sub_cap_;
    uint16_t next_sub_id_;        // monotonic; never reused while slot is in flight
};

}  // namespace mqtt_broker

#endif
