#pragma once
#include <string>
#include <unordered_map>
#include <list>
#include <vector>
#include <cstdint>
#include <cstddef>

// Wall-clock milliseconds since the Unix epoch. Must be wall-clock, not
// monotonic: expiry deadlines are written to the append-only log and have to
// still mean something after a restart.
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

    // Set expiry to an absolute wall-clock deadline rather than a duration.
    // The append-only log records deadlines this way: logging "EX 100" and
    // replaying it an hour later would wrongly revive the key with a fresh
    // 100 seconds, instead of leaving it long expired.
    bool expire_at(const std::string& k, int64_t deadline_ms);

    // The raw deadline (0 = none), for writing state out to the log.
    int64_t deadline_of(const std::string& k);

    bool persist(const std::string& k);
    int64_t ttl_ms(const std::string& k);                // -2 absent, -1 no TTL

    static constexpr int64_t kExpireBudgetUs = 1000;
    size_t active_expire_cycle(size_t samples = 20);

    // Visit every live key as fn(key, value, deadline). Skips keys whose
    // deadline has already passed but which have not been reaped yet.
    template <class F>
    void for_each(F fn) const {
        const int64_t now = now_ms();
        for (const auto& kv : map_) {
            const int64_t d = kv.second.expire_at;
            if (d != 0 && now >= d) continue;
            fn(kv.first, kv.second.value, d);
        }
    }

    size_t evicted() const { return evicted_; }
    size_t expired() const { return expired_; }

    // Keys dropped by LRU eviction since the last call, and clears the list.
    // The append-only log has to record these. Expiry does not need it -- a
    // logged absolute deadline already in the past deletes the key on replay --
    // but eviction depends on access history the log does not carry.
    std::vector<std::string> take_evicted_keys() {
        std::vector<std::string> out;
        out.swap(evicted_keys_);
        return out;
    }

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
    std::vector<std::string> evicted_keys_;
    size_t max_keys_ = 0;
    size_t evicted_  = 0;
    size_t expired_  = 0;
};
