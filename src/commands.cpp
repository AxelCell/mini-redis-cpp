#include "commands.hpp"
#include "resp.hpp"
#include <algorithm>
#include <cctype>

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

// Errors are simple strings: CRLF-terminated, NOT length-prefixed. Splicing
// client-controlled text into one lets the client inject a fake reply --
// send a command literally named "x\r\n+INJECTED" and the client library sees
// two replies where the server sent one, desynchronising every reply after it.
//
// Real Redis does the same scrub. Also cap the length so an error cannot echo
// back a megabyte of attacker data.
std::string sanitize(const std::string& s) {
    std::string r = s.substr(0, 128);
    for (char& c : r) if (c == '\r' || c == '\n') c = ' ';
    return r;
}

std::string wrong_arity(const std::string& cmd) {
    return reply_error("ERR wrong number of arguments for '" + lower(cmd) + "' command");
}

} // namespace

std::string execute(Store& store, const std::vector<std::string>& args) {
    if (args.empty()) return "";              // blank line: no reply at all

    const std::string cmd = upper(args[0]);
    const size_t argc = args.size();

    // PING           -> +PONG
    // PING <message> -> bulk echo (used to measure round-trip latency)
    if (cmd == "PING") {
        if (argc == 1) return reply_simple("PONG");
        if (argc == 2) return reply_bulk(args[1]);
        return wrong_arity(cmd);
    }

    if (cmd == "ECHO") {
        if (argc != 2) return wrong_arity(cmd);
        return reply_bulk(args[1]);
    }

    if (cmd == "SET") {
        if (argc != 3) return wrong_arity(cmd);
        store.set(args[1], args[2]);
        return reply_simple("OK");
    }

    // A miss is the NULL bulk ($-1), NOT an empty bulk ($0). Clients depend on
    // the difference: "key absent" vs "key holds an empty string".
    if (cmd == "GET") {
        if (argc != 2) return wrong_arity(cmd);
        const std::string* v = store.get(args[1]);
        return v ? reply_bulk(*v) : reply_nil();
    }

    // DEL and EXISTS are variadic and return a COUNT, not a boolean.
    // EXISTS counts duplicates: `EXISTS k k` on one existing key returns 2.
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

    if (cmd == "DBSIZE") return reply_integer(static_cast<long long>(store.size()));

    // redis-cli sends COMMAND DOCS on connect; an empty array keeps it quiet.
    if (cmd == "COMMAND") return reply_empty_array();

    if (cmd == "QUIT") return reply_simple("OK");

    return reply_error("ERR unknown command '" + sanitize(args[0]) + "'");
}
