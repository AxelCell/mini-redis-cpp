#include "aof.hpp"
#include "store.hpp"
#include "commands.hpp"
#include "resp.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>
#include <unistd.h>

int fails = 0;
static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

static std::string tmp_path(const char* tag) {
    return "/tmp/kvstore_aof_test_" + std::string(tag) + "_" +
           std::to_string(getpid()) + ".aof";
}
static size_t file_size(const std::string& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    return f.good() ? static_cast<size_t>(f.tellg()) : 0;
}
static void write_file(const std::string& p, const std::string& d) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(d.data(), static_cast<std::streamsize>(d.size()));
}

int main() {
    printf("\n=== encode_command produces valid RESP ===\n");
    {
        const std::string e = encode_command({"SET", "k", "v"});
        check(e == "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n", "SET k v encodes correctly");
        // It must round-trip through the ordinary parser: that is the whole
        // reason recovery needs no separate code path.
        std::vector<std::string> args; size_t used; std::string err;
        check(parse_command(e, args, used, err) == ParseResult::Ok &&
              args.size() == 3 && args[0] == "SET" && args[2] == "v",
              "re-parses through the network parser");
    }

    printf("\n=== write, then recover into a fresh store ===\n");
    {
        const std::string p = tmp_path("basic");
        ::unlink(p.c_str());
        {
            Store s; Aof a; std::string err;
            check(a.open(p, Aof::Sync::Always, err), "log opens");
            s.set("name", "Aishwary");     a.log_key(s, "name");
            s.set("city", "Dehradun");     a.log_key(s, "city");
            s.set("tmp", "x");             a.log_key(s, "tmp");
            s.del("tmp");                  a.log_key(s, "tmp");
            a.flush();
            a.close();
        }
        Store r; size_t applied = 0, torn = 0; std::string err;
        check(Aof::load(p, r, applied, torn, err), "log loads");
        check(torn == 0, "nothing torn in a cleanly closed log");
        check(r.size() == 2, "two keys restored, the deleted one stayed deleted");
        check(r.get("name") && *r.get("name") == "Aishwary", "value restored");
        check(r.get("tmp") == nullptr, "deleted key is absent");
        ::unlink(p.c_str());
    }

    printf("\n=== deadlines survive as absolute instants ===\n");
    {
        const std::string p = tmp_path("ttl");
        ::unlink(p.c_str());
        int64_t deadline = 0;
        {
            Store s; Aof a; std::string err;
            a.open(p, Aof::Sync::Always, err);
            s.set("k", "v", 60000);
            deadline = s.deadline_of("k");
            a.log_key(s, "k");
            a.flush(); a.close();
        }
        Store r; size_t applied = 0, torn = 0; std::string err;
        Aof::load(p, r, applied, torn, err);
        check(r.deadline_of("k") == deadline,
              "deadline restored to the same instant, not a fresh TTL");
        const int64_t left = r.ttl_ms("k");
        check(left > 0 && left <= 60000, "TTL counts down from the original");
        ::unlink(p.c_str());
    }

    printf("\n=== a deadline already past does not come back ===\n");
    {
        const std::string p = tmp_path("expired");
        ::unlink(p.c_str());
        write_file(p, encode_command({"SET", "old", "v"}) +
                      encode_command({"PEXPIREAT", "old",
                                      std::to_string(now_ms() - 10000)}));
        Store r; size_t applied = 0, torn = 0; std::string err;
        Aof::load(p, r, applied, torn, err);
        check(!r.exists("old"), "key with a past deadline stays gone after replay");
        ::unlink(p.c_str());
    }

    printf("\n=== torn tail: every truncation point recovers ===\n");
    {
        const std::string p = tmp_path("torn");
        ::unlink(p.c_str());
        std::string good;
        for (int i = 1; i <= 5; i++)
            good += encode_command({"SET", "k" + std::to_string(i),
                                          "v" + std::to_string(i)});
        // Chop one byte at a time off the end. Every prefix must load without
        // error, and must never invent or corrupt a key.
        bool all_ok = true;
        size_t worst = 0;
        for (size_t cut = 1; cut < good.size(); cut++) {
            write_file(p, good.substr(0, good.size() - cut));
            Store r; size_t applied = 0, torn = 0; std::string err;
            if (!Aof::load(p, r, applied, torn, err)) { all_ok = false; break; }
            // Whatever survived must be a correct prefix: k1..kN with right values.
            for (size_t i = 1; i <= r.size(); i++) {
                const std::string* v = r.get("k" + std::to_string(i));
                if (!v || *v != "v" + std::to_string(i)) { all_ok = false; break; }
            }
            if (r.size() > worst) worst = r.size();
        }
        check(all_ok, "all truncation points load, and never corrupt a key");
        check(worst == 4 || worst == 5, "most prefixes still recover nearly everything");
        ::unlink(p.c_str());
    }

    printf("\n=== garbage is rejected, not silently applied ===\n");
    {
        const std::string p = tmp_path("junk");
        write_file(p, "this is not RESP at all\n");
        Store r; size_t applied = 0, torn = 0; std::string err;
        check(Aof::load(p, r, applied, torn, err), "load does not fail hard");
        check(applied == 0, "nothing applied from garbage");
        check(torn > 0, "garbage reported as discarded");
        check(r.size() == 0, "store left empty");
        ::unlink(p.c_str());
    }

    printf("\n=== good records followed by garbage ===\n");
    {
        const std::string p = tmp_path("mixed");
        write_file(p, encode_command({"SET", "a", "1"}) +
                      encode_command({"SET", "b", "2"}) +
                      "TOTAL GARBAGE HERE");
        Store r; size_t applied = 0, torn = 0; std::string err;
        Aof::load(p, r, applied, torn, err);
        check(r.size() == 2, "good records kept");
        check(torn > 0, "trailing garbage discarded");
        check(file_size(p) == encode_command({"SET", "a", "1"}).size() +
                              encode_command({"SET", "b", "2"}).size(),
              "file truncated to the last clean record");
        ::unlink(p.c_str());
    }

    printf("\n=== compaction ===\n");
    {
        const std::string p = tmp_path("rewrite");
        ::unlink(p.c_str());
        Store s; Aof a; std::string err;
        a.open(p, Aof::Sync::No, err);
        for (int i = 0; i < 2000; i++) {          // one key, rewritten 2000 times
            s.set("counter", std::to_string(i));
            a.log_key(s, "counter");
        }
        s.set("keeper", "v", 60000);
        a.log_key(s, "keeper");
        a.flush();
        const size_t before = file_size(p);

        check(a.rewrite(p, s, err), "rewrite succeeds");
        const size_t after = file_size(p);
        printf("    %zu bytes -> %zu bytes\n", before, after);
        check(after < before / 10, "log shrank by more than 10x");

        Store r; size_t applied = 0, torn = 0; std::string lerr;
        Aof::load(p, r, applied, torn, lerr);
        check(r.size() == 2, "both keys present after compaction");
        check(r.get("counter") && *r.get("counter") == "1999", "final value kept");
        check(r.ttl_ms("keeper") > 0, "TTL preserved through compaction");
        a.close();
        ::unlink(p.c_str());
    }

    printf("\n=== replay is idempotent ===\n");
    {
        // Logging effects rather than commands means applying the same log
        // twice must land on exactly the same state.
        const std::string p = tmp_path("idem");
        write_file(p, encode_command({"SET", "k", "5"}) +
                      encode_command({"SET", "k", "7"}) +
                      encode_command({"DEL", "gone"}));
        Store a1; size_t ap = 0, tn = 0; std::string e;
        Aof::load(p, a1, ap, tn, e);
        Aof::load(p, a1, ap, tn, e);            // twice
        check(a1.size() == 1 && a1.get("k") && *a1.get("k") == "7",
              "replaying twice gives the same state");
        ::unlink(p.c_str());
    }

    printf("\n%s (%d failures)\n", fails ? "*** FAILURES ***" : "ALL AOF TESTS PASSED", fails);
    return fails != 0;
}
