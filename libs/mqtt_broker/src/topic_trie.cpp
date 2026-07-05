// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

#include "mqtt_broker/topic_trie.hpp"

// Topic filter trie implementation. Nodes index path segments (literal or +/#);
// SubEntry records hang off terminal nodes and carry session/QoS/options.
// match() descends the trie and re-validates with topic_matches_filter() so
// multi-level wildcards and overlapping filters are handled correctly.

#include "mqtt_broker/subscription_options.hpp"

#include <cstring>

namespace mqtt_broker {

// MQTT-5.0 §4.7.1: + and # are legal only in filters, not topic names.
static bool is_valid_topic_char(char c, bool is_filter)
{
    if (c == '\0') {
        return false;
    }
    if (c == '+' || c == '#') {
        return is_filter;
    }
    return c >= ' ' && c <= '~';
}

static const char *next_segment(const char *s)
{
    const char *slash = std::strchr(s, '/');
    return slash ? slash + 1 : nullptr;
}

static size_t segment_len(const char *s)
{
    const char *slash = std::strchr(s, '/');
    return slash ? static_cast<size_t>(slash - s) : std::strlen(s);
}

TopicTrie::TopicTrie(const TopicTrieConfig &cfg)
    : nodes_(nullptr),
      node_count_(0),
      node_cap_(cfg.max_nodes),
      root_(0),
      subs_(nullptr),
      sub_cap_(cfg.max_subscriptions),
      next_sub_id_(1)
{
    nodes_ = new Node[node_cap_];
    subs_ = new SubEntry[sub_cap_];
    std::memset(nodes_, 0, sizeof(Node) * node_cap_);
    for (size_t i = 0; i < sub_cap_; ++i) {
        subs_[i].in_use = false;
    }
    root_ = alloc_node();
}

TopicTrie::~TopicTrie()
{
    delete[] nodes_;
    delete[] subs_;
}

uint16_t TopicTrie::alloc_node()
{
    if (node_count_ >= node_cap_) {
        return 0xFFFF;
    }
    uint16_t id = static_cast<uint16_t>(node_count_++);
    nodes_[id].first_child = 0xFFFF;
    nodes_[id].next_sibling = 0xFFFF;
    nodes_[id].first_sub = 0xFFFF;
    return id;
}

bool TopicTrie::topic_valid(const char *topic)
{
    if (topic == nullptr || topic[0] == '\0' || topic[0] == '/') {
        return false;
    }
    for (const char *p = topic; *p != '\0'; ++p) {
        if (!is_valid_topic_char(*p, false)) {
            return false;
        }
    }
    return true;
}

bool TopicTrie::filter_valid(const char *filter)
{
    if (filter == nullptr || filter[0] == '\0') {
        return false;
    }
    bool hash_seen = false;
    for (const char *p = filter; *p != '\0'; ++p) {
        if (!is_valid_topic_char(*p, true)) {
            return false;
        }
        if (*p == '#') {
            if (hash_seen) {
                return false;  // MQTT-5.0 §4.7.1 — at most one multi-level wildcard
            }
            hash_seen = true;
            // # must be last segment (alone or followed only by '/')
            if (p[1] != '\0' && p[1] != '/') {
                return false;
            }
        }
        // + must occupy a whole segment
        if (*p == '+' && p[1] != '\0' && p[1] != '/') {
            return false;
        }
    }
    return true;
}

bool TopicTrie::topic_matches_filter(const char *topic, const char *filter)
{
    if (topic == nullptr || filter == nullptr) {
        return false;
    }
    if (filter[0] == '#' && filter[1] == '\0') {
        return true;
    }
    if (topic[0] == '\0' && filter[0] == '\0') {
        return true;
    }
    if (topic[0] == '\0' || filter[0] == '\0') {
        return false;
    }

    if (filter[0] == '+') {
        size_t flen = segment_len(filter);
        if (flen != 1 || (filter[1] != '\0' && filter[1] != '/')) {
            return false;
        }
        const char *tnext = next_segment(topic);
        const char *fnext = next_segment(filter);
        if (fnext == nullptr) {
            return tnext == nullptr;
        }
        return topic_matches_filter(tnext ? tnext : "", fnext);
    }

    if (filter[0] == '#') {
        return true;  // # consumes all remaining topic levels
    }

    size_t tlen = segment_len(topic);
    size_t flen = segment_len(filter);
    if (tlen != flen || std::memcmp(topic, filter, tlen) != 0) {
        return false;
    }

    const char *tnext = next_segment(topic);
    const char *fnext = next_segment(filter);
    if (fnext == nullptr) {
        return tnext == nullptr;
    }
    return topic_matches_filter(tnext ? tnext : "", fnext);
}

uint16_t TopicTrie::find_or_add_child(uint16_t parent, const char *segment, bool plus, bool hash)
{
    uint16_t child = nodes_[parent].first_child;
    while (child != 0xFFFF) {
        // One shared + or # child per parent — wildcards are not literal segments.
        if (plus && nodes_[child].is_wildcard_plus) {
            return child;
        }
        if (hash && nodes_[child].is_wildcard_hash) {
            return child;
        }
        if (!plus && !hash && std::strcmp(nodes_[child].segment, segment) == 0) {
            return child;
        }
        child = nodes_[child].next_sibling;
    }

    uint16_t id = alloc_node();
    if (id == 0xFFFF) {
        return id;
    }
    if (!plus && !hash) {
        std::strncpy(nodes_[id].segment, segment, sizeof(nodes_[id].segment) - 1);
    }
    nodes_[id].is_wildcard_plus = plus;
    nodes_[id].is_wildcard_hash = hash;
    nodes_[id].next_sibling = nodes_[parent].first_child;
    nodes_[parent].first_child = id;
    return id;
}

bool TopicTrie::subscribe(const char *filter, uint16_t session_id, uint8_t max_qos,
                          uint8_t options_byte, uint16_t *out_sub_id, bool *out_existed)
{
    if (out_existed) {
        *out_existed = false;
    }
    if (!filter_valid(filter) || !subscription_options_valid(options_byte)) {
        return false;
    }

    // Resubscribe to the same filter updates QoS/options in place (MQTT-5.0 §3.8.3.1).
    for (size_t i = 0; i < sub_cap_; ++i) {
        if (subs_[i].in_use && subs_[i].session_id == session_id &&
            std::strcmp(subs_[i].filter, filter) == 0) {
            subs_[i].max_qos = max_qos;
            subs_[i].options_byte = options_byte;
            if (out_sub_id) {
                *out_sub_id = subs_[i].sub_id;
            }
            if (out_existed) {
                *out_existed = true;
            }
            return true;
        }
    }

    size_t slot = sub_cap_;
    for (size_t i = 0; i < sub_cap_; ++i) {
        if (!subs_[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot >= sub_cap_) {
        return false;
    }

    uint16_t node = root_;
    const char *cur = filter;
    while (cur != nullptr && *cur != '\0') {
        bool plus = (cur[0] == '+');
        bool hash = (cur[0] == '#');
        char seg[256];
        if (!plus && !hash) {
            size_t len = segment_len(cur);
            if (len >= sizeof(seg)) {
                return false;
            }
            std::memcpy(seg, cur, len);
            seg[len] = '\0';
            node = find_or_add_child(node, seg, false, false);
        } else {
            node = find_or_add_child(node, "", plus, hash);
        }
        if (node == 0xFFFF) {
            return false;
        }
        nodes_[node].terminal = true;
        cur = next_segment(cur);
    }
    if (*filter == '\0') {
        nodes_[node].terminal = true;
    }

    subs_[slot].in_use = true;
    subs_[slot].sub_id = next_sub_id_++;
    subs_[slot].session_id = session_id;
    subs_[slot].max_qos = max_qos;
    subs_[slot].options_byte = options_byte;
    subs_[slot].node_id = node;
    std::strncpy(subs_[slot].filter, filter, sizeof(subs_[slot].filter) - 1);
    subs_[slot].next = nodes_[node].first_sub;
    nodes_[node].first_sub = static_cast<uint16_t>(slot);

    if (out_sub_id) {
        *out_sub_id = subs_[slot].sub_id;
    }
    return true;
}

// Removes a subscription slot from its trie node's subscriber chain. Without
// this, a freed slot that later gets reused corrupts the chain (self-cycle),
// which livelocks collect_matches().
void TopicTrie::unlink_sub(uint16_t slot)
{
    uint16_t node = subs_[slot].node_id;
    if (node == 0xFFFF || node >= node_cap_) {
        subs_[slot].next = 0xFFFF;
        return;
    }
    uint16_t *cur = &nodes_[node].first_sub;
    size_t hops = 0;
    while (*cur != 0xFFFF && hops++ < sub_cap_) {
        if (*cur == slot) {
            *cur = subs_[slot].next;
            break;
        }
        cur = &subs_[*cur].next;
    }
    subs_[slot].next = 0xFFFF;
}

bool TopicTrie::unsubscribe(const char *filter, uint16_t session_id)
{
    for (size_t i = 0; i < sub_cap_; ++i) {
        if (subs_[i].in_use && subs_[i].session_id == session_id &&
            std::strcmp(subs_[i].filter, filter) == 0) {
            unlink_sub(static_cast<uint16_t>(i));
            subs_[i].in_use = false;
            return true;
        }
    }
    return false;
}

size_t TopicTrie::subscription_count() const
{
    size_t n = 0;
    for (size_t i = 0; i < sub_cap_; ++i) {
        if (subs_[i].in_use) {
            n++;
        }
    }
    return n;
}

void TopicTrie::unsubscribe_all(uint16_t session_id)
{
    for (size_t i = 0; i < sub_cap_; ++i) {
        if (subs_[i].in_use && subs_[i].session_id == session_id) {
            unlink_sub(static_cast<uint16_t>(i));
            subs_[i].in_use = false;
        }
    }
}

void TopicTrie::collect_matches(uint16_t node, const char *topic, const char *rest,
                                TopicSubscription *out, size_t out_cap, size_t *count) const
{
    if (node == 0xFFFF) {
        return;
    }

    if (nodes_[node].is_wildcard_hash) {
        // # matches every remaining level — emit subs here, do not descend further.
        uint16_t sid = nodes_[node].first_sub;
        while (sid != 0xFFFF && *count < out_cap) {
            // Re-check full filter: trie pruning can reach this node on partial paths.
            if (subs_[sid].in_use && topic_matches_filter(topic, subs_[sid].filter)) {
                out[*count].sub_id = subs_[sid].sub_id;
                out[*count].session_id = subs_[sid].session_id;
                out[*count].max_qos = subs_[sid].max_qos;
                out[*count].options_byte = subs_[sid].options_byte;
                (*count)++;
            }
            sid = subs_[sid].next;
        }
        return;
    }

    if (rest == nullptr || *rest == '\0') {
        if (nodes_[node].terminal) {
            uint16_t sid = nodes_[node].first_sub;
            while (sid != 0xFFFF && *count < out_cap) {
                if (subs_[sid].in_use && topic_matches_filter(topic, subs_[sid].filter)) {
                    out[*count].sub_id = subs_[sid].sub_id;
                    out[*count].session_id = subs_[sid].session_id;
                    out[*count].max_qos = subs_[sid].max_qos;
                    out[*count].options_byte = subs_[sid].options_byte;
                    (*count)++;
                }
                sid = subs_[sid].next;
            }
        }
    }

    const char *next_rest = (rest != nullptr) ? next_segment(rest) : nullptr;
    uint16_t child = nodes_[node].first_child;
    while (child != 0xFFFF) {
        if (nodes_[child].is_wildcard_hash) {
            collect_matches(child, topic, "", out, out_cap, count);
        } else if (nodes_[child].is_wildcard_plus) {
            // + consumes one segment regardless of its literal value.
            collect_matches(child, topic, next_rest ? next_rest : "", out, out_cap, count);
        } else if (rest != nullptr) {
            size_t len = segment_len(rest);
            if (std::strncmp(nodes_[child].segment, rest, len) == 0 &&
                nodes_[child].segment[len] == '\0') {
                collect_matches(child, topic, next_rest, out, out_cap, count);
            }
        }
        child = nodes_[child].next_sibling;
    }
}

size_t TopicTrie::match(const char *topic, TopicSubscription *out, size_t out_cap) const
{
    size_t count = 0;
    if (!topic_valid(topic)) {
        return 0;
    }
    collect_matches(root_, topic, topic, out, out_cap, &count);
    return count;
}

}  // namespace mqtt_broker
