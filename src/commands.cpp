#include "commands.hpp"
#include "resp.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace {

std::string upper(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return r;
}
std::string lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return r;
}

// Errors are CRLF-terminated and NOT length-prefixed, so splicing client text
// into one lets the client forge a second reply and desync the stream. Scrub
// CR/LF and cap the length. Real Redis does the same.
std::string sanitize(const std::string& s) {
    std::string r = s.substr(0, 128);
    for (char& c : r) if (c == '\r' || c == '\n') c = ' ';
    return r;
}

std::string wrong_arity(const std::string& cmd) {
    return reply_error("ERR wrong number of arguments for '" + lower(cmd) + "' command");
}

// Strict: the whole token must be an integer, like the protocol parser.
bool parse_ll(const std::string& s, long long& out) {
    if (s.empty()) return false;
    size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i >= s.size()) return false;
    long long v = 0;
    for (; i < s.size(); i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        if (v > (INT64_MAX - (s[i] - '0')) / 10) return false;   // overflow guard
        v = v * 10 + (s[i] - '0');
    }
    out = (s[0] == '-') ? -v : v;
    return true;
}

const char* kNotInt = "ERR value is not an integer or out of range";

} // namespace

std::string execute(Store& store, const std::vector<std::string>& args) {
    if (args.empty()) return "";

    const std::string cmd = upper(args[0]);
    const size_t argc = args.size();

    if (cmd == "PING") {
        if (argc == 1) return reply_simple("PONG");
        if (argc == 2) return reply_bulk(args[1]);
        return wrong_arity(cmd);
    }

    if (cmd == "ECHO") {
        if (argc != 2) return wrong_arity(cmd);
        return reply_bulk(args[1]);
    }

    // SET key value [EX seconds | PX milliseconds]
    if (cmd == "SET") {
        if (argc != 3 && argc != 5) return wrong_arity(cmd);
        int64_t ttl = 0;
        if (argc == 5) {
            const std::string opt = upper(args[3]);
            long long n;
            if (!parse_ll(args[4], n)) return reply_error(kNotInt);
            if (n <= 0) return reply_error("ERR invalid expire time in 'set' command");
            if      (opt == "EX") ttl = n * 1000;
            else if (opt == "PX") ttl = n;
            else return reply_error("ERR syntax error");
        }
        store.set(args[1], args[2], ttl);
        return reply_simple("OK");
    }

    // SETEX key seconds value
    if (cmd == "SETEX") {
        if (argc != 4) return wrong_arity(cmd);
        long long n;
        if (!parse_ll(args[2], n)) return reply_error(kNotInt);
        if (n <= 0) return reply_error("ERR invalid expire time in 'setex' command");
        store.set(args[1], args[3], n * 1000);
        return reply_simple("OK");
    }

    if (cmd == "GET") {
        if (argc != 2) return wrong_arity(cmd);
        const std::string* v = store.get(args[1]);
        return v ? reply_bulk(*v) : reply_nil();      // $-1 = absent OR expired
    }

    if (cmd == "DEL") {
        if (argc < 2) return wrong_arity(cmd);
        long long n = 0;
        for (size_t i = 1; i < argc; i++) if (store.del(args[i])) n++;
        return reply_integer(n);
    }

    if (cmd == "EXISTS") {
        if (argc < 2) return wrong_arity(cmd);
        long long n = 0;
        for (size_t i = 1; i < argc; i++) if (store.exists(args[i])) n++;
        return reply_integer(n);
    }

    // EXPIRE key seconds  /  PEXPIRE key milliseconds  -> 1 set, 0 no such key
    if (cmd == "EXPIRE" || cmd == "PEXPIRE") {
        if (argc != 3) return wrong_arity(cmd);
        long long n;
        if (!parse_ll(args[2], n)) return reply_error(kNotInt);
        const int64_t ms = (cmd == "EXPIRE") ? n * 1000 : n;
        // A non-positive TTL deletes the key immediately, as Redis does.
        if (ms <= 0) return reply_integer(store.del(args[1]) ? 1 : 0);
        return reply_integer(store.expire(args[1], ms) ? 1 : 0);
    }

    // TTL  -> seconds remaining, -1 = no TTL, -2 = no such key
    // PTTL -> same in milliseconds
    if (cmd == "TTL" || cmd == "PTTL") {
        if (argc != 2) return wrong_arity(cmd);
        const int64_t ms = store.ttl_ms(args[1]);
        if (ms < 0) return reply_integer(ms);                 // -1 or -2 pass through
        return reply_integer(cmd == "TTL" ? (ms + 999) / 1000 : ms);  // round up
    }

    if (cmd == "PERSIST") {
        if (argc != 2) return wrong_arity(cmd);
        return reply_integer(store.persist(args[1]) ? 1 : 0);
    }

    if (cmd == "DBSIZE") return reply_integer(static_cast<long long>(store.size()));

    if (cmd == "INFO") {
        std::string s;
        s += "# Keyspace\r\n";
        s += "keys:"    + std::to_string(store.size())    + "\r\n";
        s += "expired:" + std::to_string(store.expired()) + "\r\n";
        s += "evicted:" + std::to_string(store.evicted()) + "\r\n";
        return reply_bulk(s);
    }

    if (cmd == "COMMAND") return reply_empty_array();
    if (cmd == "QUIT")    return reply_simple("OK");

    return reply_error("ERR unknown command '" + sanitize(args[0]) + "'");
}
