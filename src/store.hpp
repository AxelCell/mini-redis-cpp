#pragma once
#include <string>
#include <unordered_map>
#include <list>
#include <vector>
#include <cstdint>
#include <cstddef>

// Milliseconds from a monotonic clock. Deliberately NOT wall-clock: if the
// system clock jumps (NTP correction, user change), wall-clock TTLs would
// expire early or late. Durations must come from a clock that only moves
// forward.
int64_t now_ms();

// ---------------------------------------------------------------------------
// The key-value store: hash map + LRU list + per-key expiry.
//
// LRU is the classic pairing:
//   unordered_map  -> O(1) lookup by key
//   std::list      -> O(1) move-to-front on access, O(1) evict from the back
// Each map entry stores an ITERATOR into the list. std::list iterators stay
// valid across splice() and other insertions, which is exactly what makes the
// move-to-front O(1) -- no search required.
// ---------------------------------------------------------------------------
class Store {
public:
    // max_keys == 0 means unlimited (no eviction).
    explicit Store(size_t max_keys = 0) : max_keys_(max_keys) {}

    // ttl_ms == 0 means "no expiry".
    void set(const std::string& k, const std::string& v, int64_t ttl_ms = 0);

    // NOTE: not const. A read can DELETE the key (lazy expiry), and it also
    // reorders the LRU list. A getter that mutates is surprising, so it is
    // worth saying out loud: in a cache, reading is a write.
    const std::string* get(const std::string& k);

    bool   del(const std::string& k);
    bool   exists(const std::string& k);
    size_t size() const { return map_.size(); }   // may include not-yet-reaped keys

    bool expire(const std::string& k, int64_t ttl_ms);   // false if key absent
    bool persist(const std::string& k);                  // strip the TTL
    int64_t ttl_ms(const std::string& k);                // -2 absent, -1 no TTL

    // Redis-style active expiry: sample random keys that carry a TTL, delete
    // the expired ones, and repeat while the hit rate stays high. Bounded by
    // both a round cap AND a wall-clock budget so a single cycle can never
    // stall the event loop.
    static constexpr int64_t kExpireBudgetUs = 1000;   // 1 ms per cron tick
    size_t active_expire_cycle(size_t samples = 20);

    size_t evicted() const { return evicted_; }
    size_t expired() const { return expired_; }

private:
    static constexpr size_t kNoVol = static_cast<size_t>(-1);

    struct Entry {
        std::string value;
        int64_t expire_at = 0;                       // 0 = never expires
        std::list<std::string>::iterator lru;        // this key's node in lru_
        size_t  vol_idx = kNoVol;                    // index into vol_, or kNoVol
    };
    using Map = std::unordered_map<std::string, Entry>;

    void erase_entry(Map::iterator it);
    void vol_add(Map::iterator it);                  // register a key as having a TTL
    void vol_remove(Map::iterator it);               // deregister (O(1) swap-with-last)
    bool expire_if_needed(Map::iterator it);         // lazy expiry; true if reaped
    void evict_if_needed();

    Map map_;
    std::list<std::string> lru_;   // front = most recently used, back = victim

    // Keys that currently carry a TTL -- the equivalent of Redis's separate
    // `expires` dict. Sampling for active expiry draws from HERE, not from the
    // main table, so every sample is a genuine candidate. Sampling the main
    // table instead means that once most keys are gone you are probing a huge
    // mostly-empty bucket array, and the drain tail becomes quadratic-ish.
    // Entry::vol_idx is this key's slot, which makes removal O(1).
    std::vector<std::string> vol_;
    size_t max_keys_ = 0;
    size_t evicted_  = 0;
    size_t expired_  = 0;
};
