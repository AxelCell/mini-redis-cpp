#include "commands.hpp"
#include "store.hpp"
#include <cstdio>
#include <string>
#include <vector>
int fails = 0;
static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}
static std::string run(Store& s, std::vector<std::string> args) {
    return execute(s, args);
}

int main() {
    printf("\n=== INCR / DECR ===\n");
    {
        Store s;
        check(run(s, {"INCR", "c"}) == ":1\r\n",     "INCR on missing key -> 1");
        check(run(s, {"INCR", "c"}) == ":2\r\n",     "INCR again -> 2");
        check(run(s, {"INCRBY", "c", "10"}) == ":12\r\n", "INCRBY 10 -> 12");
        check(run(s, {"DECR", "c"}) == ":11\r\n",    "DECR -> 11");
        check(run(s, {"DECRBY", "c", "5"}) == ":6\r\n",   "DECRBY 5 -> 6");
        check(run(s, {"GET", "c"}) == "$1\r\n6\r\n", "value stored as a string");
        check(run(s, {"DECRBY", "c", "-4"}) == ":10\r\n", "DECRBY negative adds");
    }

    printf("\n=== INCR type and overflow errors ===\n");
    {
        Store s;
        run(s, {"SET", "w", "hello"});
        check(run(s, {"INCR", "w"}).rfind("-ERR", 0) == 0, "INCR on non-numeric -> error");
        check(run(s, {"INCRBY", "w", "abc"}).rfind("-ERR", 0) == 0, "non-numeric delta -> error");
        run(s, {"SET", "big", "9223372036854775807"});
        check(run(s, {"INCR", "big"}).rfind("-ERR", 0) == 0, "INT64_MAX + 1 rejected, not wrapped");
        run(s, {"SET", "small", "-9223372036854775808"});
        check(run(s, {"DECR", "small"}).rfind("-ERR", 0) == 0, "INT64_MIN - 1 rejected");
        // The value must be unchanged after a rejected overflow.
        check(run(s, {"GET", "big"}) == "$19\r\n9223372036854775807\r\n", "value intact after rejection");
    }

    printf("\n=== INCR preserves TTL (the rate-limiter requirement) ===\n");
    {
        Store s;
        run(s, {"SET", "rl", "0", "EX", "100"});
        run(s, {"INCR", "rl"});
        run(s, {"INCR", "rl"});
        const std::string ttl = run(s, {"TTL", "rl"});
        printf("    TTL after 2 INCRs: %s", ttl.c_str());
        check(ttl != ":-1\r\n", "TTL was NOT cleared by INCR");
        check(run(s, {"GET", "rl"}) == "$1\r\n2\r\n", "counter incremented to 2");
        // ...whereas a plain SET *does* clear it, per Redis semantics.
        run(s, {"SET", "rl", "0"});
        check(run(s, {"TTL", "rl"}) == ":-1\r\n", "plain SET does clear the TTL");
    }

    printf("\n=== INCR on a new key has no TTL ===\n");
    {
        Store s;
        run(s, {"INCR", "fresh"});
        check(run(s, {"TTL", "fresh"}) == ":-1\r\n", "new counter is persistent until EXPIREd");
    }

    printf("\n=== arity ===\n");
    {
        Store s;
        check(run(s, {"INCR"}).rfind("-ERR", 0) == 0,              "INCR with no key -> error");
        check(run(s, {"INCR", "a", "b"}).rfind("-ERR", 0) == 0,    "INCR with 2 args -> error");
        check(run(s, {"INCRBY", "a"}).rfind("-ERR", 0) == 0,       "INCRBY missing delta -> error");
    }

    printf("\n%s (%d failures)\n", fails ? "*** FAILURES ***" : "ALL COMMAND TESTS PASSED", fails);
    return fails != 0;
}
