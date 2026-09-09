#pragma once
#include <string>
#include <unordered_map>

// The key-value store.
//
// Phase 2 is deliberately just std::unordered_map -- a hash table with
// chaining, average O(1) lookup. Phase 4 replaces it with our own table so we
// control incremental rehashing (std::unordered_map rehashes in one
// stop-the-world pass, which spikes p99 latency) and per-entry TTL/LRU data.
//
// Hiding it behind this interface means that change touches one file.
class Store {
public:
    void set(const std::string& k, const std::string& v) { map_[k] = v; }

    // nullptr on miss -- avoids copying the value just to signal absence.
    const std::string* get(const std::string& k) const {
        auto it = map_.find(k);
        return it == map_.end() ? nullptr : &it->second;
    }

    bool   del(const std::string& k)          { return map_.erase(k) > 0; }
    bool   exists(const std::string& k) const { return map_.count(k) > 0; }
    size_t size() const                       { return map_.size(); }

private:
    std::unordered_map<std::string, std::string> map_;
};
