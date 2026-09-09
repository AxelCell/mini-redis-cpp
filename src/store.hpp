#pragma once
#include <string>
#include <unordered_map>
#include <list>
#include <vector>
#include <cstdint>
#include <cstddef>

// Monotonic, not wall-clock: an NTP correction must not expire keys early.
int64_t now_ms();

// Hash map for O(1) lookup, plus a list holding keys in use order. Each entry
// keeps an iterator to its own list node, so move-to-front and evict-from-back
// are O(1) with no search.
class Store {
public:
    explicit Store(size_t max_keys = 0) : max_keys_(max_keys) {}   // 0 = unlimited

    void set(const std::string& k, const std::string& v, int64_t ttl_ms = 0);

    // Not const: a read may delete an expired key and reorders the LRU list.
    const std::string* get(const std::string& k);

    // Mutable value access that leaves expire_at alone, so INCR does not reset
    // a key's TTL. Returns nullptr if absent or expired.
    std::string* get_mut(const std::string& k);

    bool   del(const std::string& k);
    bool   exists(const std::string& k);
    size_t size() const { return map_.size(); }

    bool expire(const std::string& k, int64_t ttl_ms);
    bool persist(const std::string& k);
    int64_t ttl_ms(const std::string& k);                // -2 absent, -1 no TTL

    static constexpr int64_t kExpireBudgetUs = 1000;
    size_t active_expire_cycle(size_t samples = 20);

    size_t evicted() const { return evicted_; }
    size_t expired() const { return expired_; }

private:
    static constexpr size_t kNoVol = static_cast<size_t>(-1);

    struct Entry {
        std::string value;
        int64_t expire_at = 0;                       // 0 = never
        std::list<std::string>::iterator lru;
        size_t  vol_idx = kNoVol;                    // slot in vol_
    };
    using Map = std::unordered_map<std::string, Entry>;

    void erase_entry(Map::iterator it);
    void vol_add(Map::iterator it);
    void vol_remove(Map::iterator it);
    bool expire_if_needed(Map::iterator it);
    void evict_if_needed();

    Map map_;
    std::list<std::string> lru_;   // front = newest use, back = eviction victim

    // Only the keys that carry a TTL. Active expiry samples from here rather
    // than from map_: unordered_map never shrinks its bucket array, so once
    // most keys are gone, random bucket probes almost always land on empty
    // buckets and the cleanup tail collapses.
    std::vector<std::string> vol_;
    size_t max_keys_ = 0;
    size_t evicted_  = 0;
    size_t expired_  = 0;
};
