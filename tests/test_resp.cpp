#include "resp.hpp"
#include <cstdio>
#include <string>
#include <vector>
int fails = 0;
static std::string show(const std::vector<std::string>& a) {
    std::string s = "[";
    for (size_t i=0;i<a.size();i++){ if(i) s += ", "; s += "\"" + a[i] + "\""; }
    return s + "]";
}
static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

int main() {
    std::vector<std::string> args; size_t used; std::string err;

    printf("\n=== 1. complete command ===\n");
    std::string b = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
    auto r = parse_command(b, args, used, err);
    printf("  parsed %s, consumed %zu of %zu\n", show(args).c_str(), used, b.size());
    check(r==ParseResult::Ok && args.size()==3 && args[0]=="SET" && args[2]=="bar", "SET foo bar");
    check(used == b.size(), "consumed entire buffer");

    printf("\n=== 2. BYTE-BY-BYTE delivery (the partial-read torture test) ===\n");
    std::string full = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
    std::string acc;
    int okAt = -1; bool everConsumedEarly = false;
    for (size_t i = 0; i < full.size(); i++) {
        acc += full[i];
        r = parse_command(acc, args, used, err);
        if (r == ParseResult::NeedMore && used != 0) everConsumedEarly = true;
        if (r == ParseResult::Ok && okAt < 0) okAt = (int)i + 1;
        if (r == ParseResult::Error) { printf("  unexpected Error at byte %zu\n", i); fails++; }
    }
    printf("  first reported Ok after byte %d (buffer is %zu bytes)\n", okAt, full.size());
    check(okAt == (int)full.size(), "Ok ONLY when the last byte arrives");
    check(!everConsumedEarly, "never consumed a byte while returning NeedMore");

    printf("\n=== 3. pipelining: 2 commands in one buffer ===\n");
    b = "*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nGET\r\n$1\r\nk\r\n";
    r = parse_command(b, args, used, err);
    printf("  1st: %s consumed %zu\n", show(args).c_str(), used);
    check(r==ParseResult::Ok && args.size()==1 && args[0]=="PING", "first is PING");
    b.erase(0, used);
    r = parse_command(b, args, used, err);
    printf("  2nd: %s consumed %zu\n", show(args).c_str(), used);
    check(r==ParseResult::Ok && args.size()==2 && args[1]=="k", "second is GET k");
    b.erase(0, used);
    check(b.empty(), "buffer fully drained");

    printf("\n=== 4. binary safety: value contains CRLF and a NUL ===\n");
    std::string val = std::string("a\r\nb") + '\0' + "c";   // 7 bytes
    b = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$" + std::to_string(val.size()) + "\r\n" + val + "\r\n";
    r = parse_command(b, args, used, err);
    check(r==ParseResult::Ok && args.size()==3 && args[2]==val && args[2].size()==val.size(),
          "payload with CRLF+NUL survives byte-exact");

    printf("\n=== 5. inline commands (what nc sends) ===\n");
    b = "SET foo bar\n";
    r = parse_command(b, args, used, err);
    printf("  %s\n", show(args).c_str());
    check(r==ParseResult::Ok && args.size()==3 && args[2]=="bar", "inline with \\n");
    b = "PING\r\n";
    r = parse_command(b, args, used, err);
    check(r==ParseResult::Ok && args.size()==1 && args[0]=="PING", "inline with \\r\\n");
    b = "PING";                              // no newline yet
    r = parse_command(b, args, used, err);
    check(r==ParseResult::NeedMore, "inline without newline -> NeedMore");

    printf("\n=== 6. protocol errors are rejected, not guessed ===\n");
    struct { const char* in; const char* why; } bad[] = {
        {"*2\r\n$3\r\nGET\r\n+foo\r\n",  "element not a bulk string"},
        {"*abc\r\n",                      "non-numeric array length"},
        {"*2\r\n$xy\r\n",                 "non-numeric bulk length"},
        {"*2\r\n$-5\r\n",                 "negative bulk length"},
    };
    for (auto& t : bad) {
        std::string s(t.in);
        r = parse_command(s, args, used, err);
        printf("  %-28s -> %s\n", t.why, err.c_str());
        check(r==ParseResult::Error && used==0, t.why);
    }

    printf("\n%s (%d failures)\n", fails ? "*** FAILURES ***" : "ALL TESTS PASSED", fails);
    return fails != 0;
}
