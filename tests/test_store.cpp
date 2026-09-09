#include "store.hpp"
#include <cstdio>
#include <thread>
#include <chrono>
#include <vector>
#include <unordered_map>
#include <random>
#include <string>
int fails = 0;
static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}
static void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int main() {
    printf("\n=== TTL: basics ===\n");
    {
        Store s;
        s.set("a", "1");
        check(s.ttl_ms("a") == -1, "no TTL -> -1");
        check(s.ttl_ms("ghost") == -2, "absent key -> -2");
        check(s.expire("a", 5000), "EXPIRE on existing key -> true");
        int64_t t = s.ttl_ms("a");
        check(t > 4000 && t <= 5000, "TTL reports remaining ms");
        check(s.persist("a"), "PERSIST strips the TTL");
        check(s.ttl_ms("a") == -1, "after PERSIST -> -1");
        check(!s.persist("a"), "PERSIST again -> false (no TTL to strip)");
        check(!s.expire("ghost", 1000), "EXPIRE on absent key -> false");
    }

    printf("\n=== TTL: lazy expiry on access ===\n");
    {
        Store s;
        s.set("k", "v", 80);                 // 80 ms
        check(s.get("k") != nullptr, "present before expiry");
        sleep_ms(150);
        check(s.get("k") == nullptr, "GET after expiry -> miss");
        check(!s.exists("k"), "EXISTS after expiry -> false");
        check(s.ttl_ms("k") == -2, "TTL after expiry -> -2");
        check(s.expired() >= 1, "expired counter incremented");
    }

    printf("\n=== TTL: SET clears an existing TTL (Redis semantics) ===\n");
    {
        Store s;
        s.set("k", "v", 5000);
        check(s.ttl_ms("k") > 0, "has TTL");
        s.set("k", "v2");                    // plain SET
        check(s.ttl_ms("k") == -1, "plain SET removed the TTL");
    }

    printf("\n=== TTL: active expiry reaps UNTOUCHED keys ===\n");
    {
        Store s;
        for (int i = 0; i < 200; i++) s.set("k" + std::to_string(i), "v", 50);
        check(s.size() == 200, "200 keys inserted");
        sleep_ms(120);
        check(s.size() == 200, "still 200: nothing has touched them yet");
        size_t before = s.size();
        for (int i = 0; i < 40; i++) s.active_expire_cycle();
        printf("    size %zu -> %zu after cycles\n", before, s.size());
        check(s.size() < before, "active expiry removed untouched expired keys");
    }

    printf("\n=== LRU: eviction order ===\n");
    {
        Store s(3);                          // capacity 3
        s.set("a", "1"); s.set("b", "2"); s.set("c", "3");
        check(s.size() == 3, "at capacity");
        s.get("a");                          // touch 'a' -> now most recent; 'b' is LRU
        s.set("d", "4");                     // over capacity -> evict LRU
        check(s.size() == 3, "still at capacity after insert");
        check(s.exists("a"), "'a' survived (recently read)");
        check(!s.exists("b"), "'b' evicted (least recently used)");
        check(s.exists("c") && s.exists("d"), "'c' and 'd' present");
        check(s.evicted() == 1, "evicted counter == 1");
    }

    printf("\n=== LRU: writing also counts as use ===\n");
    {
        Store s(2);
        s.set("x", "1"); s.set("y", "2");
        s.set("x", "1b");                    // rewrite x -> x is now MRU
        s.set("z", "3");                     // evicts y
        check(s.exists("x") && s.exists("z"), "x and z present");
        check(!s.exists("y"), "y evicted");
    }

    printf("\n=== LRU: unlimited by default ===\n");
    {
        Store s;
        for (int i = 0; i < 1000; i++) s.set("k" + std::to_string(i), "v");
        check(s.size() == 1000, "no eviction when max_keys == 0");
        check(s.evicted() == 0, "evicted counter still 0");
    }

    printf("\n=== TTL-set invariant under random churn ===\n");
    {
        // The active-expiry sample set (vol_) is maintained with a
        // swap-with-last removal that has to fix up the moved key's recorded
        // index. That is the easiest place for a subtle bug to hide, so hammer
        // every path that adds or removes TTL membership and check the store
        // still agrees with an independent model.
        Store s;
        std::unordered_map<std::string, bool> has_ttl;   // model: key -> has a TTL
        std::mt19937 rng(12345);
        for (int i = 0; i < 20000; i++) {
            std::string k = "k" + std::to_string(rng() % 200);
            switch (rng() % 6) {
                case 0: s.set(k, "v");            has_ttl[k] = false; break;
                case 1: s.set(k, "v", 100000);    has_ttl[k] = true;  break;
                case 2: if (s.expire(k, 100000))  has_ttl[k] = true;  break;
                case 3: if (s.persist(k))         has_ttl[k] = false; break;
                case 4: if (s.del(k))             has_ttl.erase(k);   break;
                case 5: s.active_expire_cycle();                      break;
            }
        }
        size_t mismatches = 0, model_ttls = 0;
        for (auto& kv : has_ttl) {
            const int64_t t = s.ttl_ms(kv.first);
            const bool store_says_ttl = (t >= 0);
            if (t == -2) continue;                       // key gone; fine
            if (kv.second) model_ttls++;
            if (store_says_ttl != kv.second) mismatches++;
        }
        printf("    %zu keys modelled with a TTL, %zu mismatches\n", model_ttls, mismatches);
        check(mismatches == 0, "TTL membership matches the model after 20k random ops");
        check(s.size() <= 200, "no key leakage");
        // Nothing expired (all TTLs were 100s), so a cycle must remove nothing.
        check(s.active_expire_cycle() == 0, "cycle removes nothing when nothing is due");
    }

    printf("\n%s (%d failures)\n", fails ? "*** FAILURES ***" : "ALL STORE TESTS PASSED", fails);
    return fails != 0;
}
