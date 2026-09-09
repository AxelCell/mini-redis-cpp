#include "store.hpp"
#include <chrono>
#include <random>
#include <vector>

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Register a key as carrying a TTL. Idempotent.
void Store::vol_add(Map::iterator it) {
    if (it->second.vol_idx != kNoVol) return;
    vol_.push_back(it->first);
    it->second.vol_idx = vol_.size() - 1;
}

// Deregister in O(1): move the last element into this slot and shrink. The
// key that got moved must have its recorded index fixed up, which is why
// Entry stores the index rather than us searching for it.
void Store::vol_remove(Map::iterator it) {
    const size_t idx = it->second.vol_idx;
    if (idx == kNoVol) return;
    const size_t last = vol_.size() - 1;
    if (idx != last) {
        vol_[idx] = std::move(vol_[last]);
        auto moved = map_.find(vol_[idx]);
        if (moved != map_.end()) moved->second.vol_idx = idx;
    }
    vol_.pop_back();
    it->second.vol_idx = kNoVol;
}

void Store::erase_entry(Map::iterator it) {
    vol_remove(it);                 // keep the TTL set in sync
    lru_.erase(it->second.lru);     // O(1): we hold the node's iterator
    map_.erase(it);
}

// Lazy expiry: called on every access. Returns true if the key was reaped,
// in which case `it` is invalid.
bool Store::expire_if_needed(Map::iterator it) {
    if (it == map_.end()) return false;
    const int64_t exp = it->second.expire_at;
    if (exp == 0 || now_ms() < exp) return false;
    erase_entry(it);
    expired_++;
    return true;
}

// Evict from the back of the LRU list until we are under the limit.
void Store::evict_if_needed() {
    while (max_keys_ != 0 && map_.size() > max_keys_ && !lru_.empty()) {
        // Copy the key: lru_.back() is a reference INTO the node we erase.
        const std::string victim = lru_.back();
        auto it = map_.find(victim);
        if (it == map_.end()) { lru_.pop_back(); continue; }   // shouldn't happen
        erase_entry(it);
        evicted_++;
    }
}

void Store::set(const std::string& k, const std::string& v, int64_t ttl_ms) {
    auto it = map_.find(k);
    if (it != map_.end()) {
        it->second.value = v;
        // Redis semantics: a plain SET clears any existing TTL.
        it->second.expire_at = (ttl_ms > 0) ? now_ms() + ttl_ms : 0;
        if (ttl_ms > 0) vol_add(it); else vol_remove(it);
        lru_.splice(lru_.begin(), lru_, it->second.lru);   // move-to-front, O(1)
        return;
    }
    lru_.push_front(k);
    Entry e;
    e.value     = v;
    e.expire_at = (ttl_ms > 0) ? now_ms() + ttl_ms : 0;
    e.lru       = lru_.begin();
    auto ins = map_.emplace(k, std::move(e));
    if (ttl_ms > 0) vol_add(ins.first);
    evict_if_needed();
}

const std::string* Store::get(const std::string& k) {
    auto it = map_.find(k);
    if (it == map_.end()) return nullptr;
    if (expire_if_needed(it)) return nullptr;          // was expired: a miss
    lru_.splice(lru_.begin(), lru_, it->second.lru);   // reading counts as use
    return &it->second.value;
}

std::string* Store::get_mut(const std::string& k) {
    auto it = map_.find(k);
    if (it == map_.end()) return nullptr;
    if (expire_if_needed(it)) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second.lru);   // counts as use
    return &it->second.value;                          // expire_at untouched
}

bool Store::del(const std::string& k) {
    auto it = map_.find(k);
    if (it == map_.end()) return false;
    if (expire_if_needed(it)) return false;   // already logically gone
    erase_entry(it);
    return true;
}

bool Store::exists(const std::string& k) {
    auto it = map_.find(k);
    if (it == map_.end()) return false;
    return !expire_if_needed(it);
}

bool Store::expire(const std::string& k, int64_t ttl) {
    auto it = map_.find(k);
    if (it == map_.end()) return false;
    if (expire_if_needed(it)) return false;
    it->second.expire_at = now_ms() + ttl;
    vol_add(it);
    return true;
}

bool Store::persist(const std::string& k) {
    auto it = map_.find(k);
    if (it == map_.end()) return false;
    if (expire_if_needed(it)) return false;
    if (it->second.expire_at == 0) return false;   // had no TTL to remove
    it->second.expire_at = 0;
    vol_remove(it);
    return true;
}

int64_t Store::ttl_ms(const std::string& k) {
    auto it = map_.find(k);
    if (it == map_.end()) return -2;              // no such key
    if (expire_if_needed(it)) return -2;
    if (it->second.expire_at == 0) return -1;     // exists, but no TTL
    int64_t left = it->second.expire_at - now_ms();
    return left > 0 ? left : 0;
}

// Redis's approach: sample a batch of random keys, delete the expired ones,
// and if MORE THAN 25% of the batch was expired, go round again -- lots of
// garbage means it is worth continuing; little garbage means stop and give the
// event loop back. The round cap bounds worst-case latency.
size_t Store::active_expire_cycle(size_t samples) {
    if (vol_.empty()) return 0;

    static std::mt19937 rng{std::random_device{}()};
    const int64_t now = now_ms();
    size_t total_removed = 0;

    // Redis's adaptive loop: sample a batch from the TTL set, delete whatever
    // has expired, and go round again only if MORE THAN 25% of the batch was
    // expired. Lots of garbage means the extra work pays; little garbage means
    // stop and hand the event loop back.
    //
    // The round cap alone is NOT enough. Measured draining 100k expired keys,
    // a 16-round cycle took up to 1.68 ms -- longer than the p50 request
    // latency under load, so every client felt the hiccup. A single-threaded
    // event loop cannot afford that, so the cycle is also bounded by WALL TIME
    // and gives the loop back when the budget is spent. Redis does the same.
    const auto t_start = std::chrono::steady_clock::now();

    for (int round = 0; round < 16 && !vol_.empty(); round++) {
        const size_t n = std::min(samples, vol_.size());
        size_t found = 0;

        for (size_t i = 0; i < n && !vol_.empty(); i++) {
            // Every draw is a key that HAS a TTL, so no sample is wasted.
            const std::string k = vol_[rng() % vol_.size()];   // copy: erase moves slots
            auto it = map_.find(k);
            if (it == map_.end()) continue;                    // defensive
            if (it->second.expire_at != 0 && now >= it->second.expire_at) {
                erase_entry(it);
                expired_++;
                total_removed++;
                found++;
            }
        }

        if (found * 4 < n) break;      // under 25% expired: table is mostly clean

        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - t_start).count();
        if (elapsed >= kExpireBudgetUs) break;   // out of time: resume next cron tick
    }
    return total_removed;
}
