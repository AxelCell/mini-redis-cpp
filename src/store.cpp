#include "store.hpp"
#include <chrono>
#include <random>
#include <vector>

// Wall-clock, not monotonic. A monotonic clock counts from an arbitrary point
// and restarts with the machine, so a deadline written to disk would be
// meaningless after a reboot -- persistence needs timestamps that survive one.
// The cost is that a large clock correction shifts expiry times; Redis makes
// the same trade for the same reason.
//
// Note this is only for DEADLINES. Measuring an elapsed duration (the expiry
// cycle's time budget below) still uses steady_clock, which cannot jump.
int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void Store::vol_add(Map::iterator it) {
    if (it->second.vol_idx != kNoVol) return;
    vol_.push_back(it->first);
    it->second.vol_idx = vol_.size() - 1;
}

// Swap the last element into this slot and shrink. The key that moved needs
// its recorded index corrected, which is why Entry stores the index at all.
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
    vol_remove(it);
    lru_.erase(it->second.lru);
    map_.erase(it);
}

// Returns true if the key was reaped, in which case `it` is invalid.
bool Store::expire_if_needed(Map::iterator it) {
    if (it == map_.end()) return false;
    const int64_t exp = it->second.expire_at;
    if (exp == 0 || now_ms() < exp) return false;
    erase_entry(it);
    expired_++;
    return true;
}

void Store::evict_if_needed() {
    while (max_keys_ != 0 && map_.size() > max_keys_ && !lru_.empty()) {
        const std::string victim = lru_.back();   // copy: back() points into the node we erase
        auto it = map_.find(victim);
        if (it == map_.end()) { lru_.pop_back(); continue; }
        erase_entry(it);
        evicted_keys_.push_back(victim);
        evicted_++;
    }
}

void Store::set(const std::string& k, const std::string& v, int64_t ttl_ms) {
    auto it = map_.find(k);
    if (it != map_.end()) {
        it->second.value = v;
        it->second.expire_at = (ttl_ms > 0) ? now_ms() + ttl_ms : 0;   // plain SET clears the TTL
        if (ttl_ms > 0) vol_add(it); else vol_remove(it);
        lru_.splice(lru_.begin(), lru_, it->second.lru);
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
    if (expire_if_needed(it)) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second.lru);   // reading counts as use
    return &it->second.value;
}

std::string* Store::get_mut(const std::string& k) {
    auto it = map_.find(k);
    if (it == map_.end()) return nullptr;
    if (expire_if_needed(it)) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    return &it->second.value;                          // expire_at left alone
}

bool Store::del(const std::string& k) {
    auto it = map_.find(k);
    if (it == map_.end()) return false;
    if (expire_if_needed(it)) return false;
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

bool Store::expire_at(const std::string& k, int64_t deadline_ms) {
    auto it = map_.find(k);
    if (it == map_.end()) return false;
    if (expire_if_needed(it)) return false;
    it->second.expire_at = deadline_ms;
    vol_add(it);
    return true;
}

int64_t Store::deadline_of(const std::string& k) {
    auto it = map_.find(k);
    if (it == map_.end()) return 0;
    if (expire_if_needed(it)) return 0;
    return it->second.expire_at;
}

bool Store::persist(const std::string& k) {
    auto it = map_.find(k);
    if (it == map_.end()) return false;
    if (expire_if_needed(it)) return false;
    if (it->second.expire_at == 0) return false;
    it->second.expire_at = 0;
    vol_remove(it);
    return true;
}

int64_t Store::ttl_ms(const std::string& k) {
    auto it = map_.find(k);
    if (it == map_.end()) return -2;
    if (expire_if_needed(it)) return -2;
    if (it->second.expire_at == 0) return -1;
    int64_t left = it->second.expire_at - now_ms();
    return left > 0 ? left : 0;
}

// Sample a batch of keys that have a TTL, delete the expired ones, and only go
// round again if more than 25% of the batch was expired. Also bounded by wall
// time: draining 100k keys took up to 1.68 ms per cycle, which is longer than
// a request takes, and a single-threaded loop cannot stall for that long.
size_t Store::active_expire_cycle(size_t samples) {
    if (vol_.empty()) return 0;

    static std::mt19937 rng{std::random_device{}()};
    const int64_t now = now_ms();
    const auto t_start = std::chrono::steady_clock::now();
    size_t total_removed = 0;

    for (int round = 0; round < 16 && !vol_.empty(); round++) {
        const size_t n = std::min(samples, vol_.size());
        size_t found = 0;

        for (size_t i = 0; i < n && !vol_.empty(); i++) {
            const std::string k = vol_[rng() % vol_.size()];   // copy: erase moves slots
            auto it = map_.find(k);
            if (it == map_.end()) continue;
            if (it->second.expire_at != 0 && now >= it->second.expire_at) {
                erase_entry(it);
                expired_++;
                total_removed++;
                found++;
            }
        }

        if (found * 4 < n) break;

        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - t_start).count();
        if (elapsed >= kExpireBudgetUs) break;
    }
    return total_removed;
}
