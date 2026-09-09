#include "resp.hpp"
#include <sstream>

namespace {

// Caps so a hostile client cannot make us allocate without bound.
constexpr size_t    kMaxInlineLen = 64 * 1024;
constexpr long long kMaxArgs      = 1024 * 1024;
constexpr long long kMaxBulkLen   = 512LL * 1024 * 1024;

// false means the line is not fully buffered yet, which is NeedMore, not an
// error. Treating a slow network as a protocol violation drops good clients.
bool read_line(const std::string& b, size_t& pos, std::string& line) {
    size_t nl = b.find("\r\n", pos);
    if (nl == std::string::npos) return false;
    line.assign(b, pos, nl - pos);
    pos = nl + 2;
    return true;
}

// The whole string must be digits. atoll("12abc") would silently return 12,
// and atoll("abc") returns 0 -- indistinguishable from a real zero.
bool to_ll(const std::string& s, long long& out) {
    if (s.empty()) return false;
    size_t i = 0;
    bool neg = (s[0] == '-');
    if (neg || s[0] == '+') i = 1;
    if (i >= s.size()) return false;
    long long v = 0;
    for (; i < s.size(); i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
        if (v > kMaxBulkLen * 4) return false;   // signed overflow is UB
    }
    out = neg ? -v : v;
    return true;
}

} // namespace

ParseResult parse_command(const std::string& buf,
                          std::vector<std::string>& args,
                          size_t& consumed,
                          std::string& err) {
    args.clear();
    consumed = 0;
    err.clear();

    if (buf.empty()) return ParseResult::NeedMore;

    // Inline command: a bare line like "SET foo bar\n", which is what nc and
    // telnet send. Handy for testing without a RESP client.
    if (buf[0] != '*') {
        size_t nl = buf.find('\n');
        if (nl == std::string::npos) {
            if (buf.size() > kMaxInlineLen) {
                err = "ERR Protocol error: too big inline request";
                return ParseResult::Error;
            }
            return ParseResult::NeedMore;
        }
        std::string line(buf, 0, nl);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        consumed = nl + 1;

        std::istringstream iss(line);
        std::string word;
        while (iss >> word) args.push_back(word);
        return ParseResult::Ok;                   // may be empty: blank line
    }

    // RESP array. `pos` is a cursor; `consumed` is only published at the very
    // end, so every early return leaves the caller's buffer untouched.
    size_t pos = 0;
    std::string line;

    if (!read_line(buf, pos, line)) {
        if (buf.size() > kMaxInlineLen) {
            err = "ERR Protocol error: too big mbulk count string";
            return ParseResult::Error;
        }
        return ParseResult::NeedMore;
    }

    long long n;
    if (!to_ll(line.substr(1), n) || n > kMaxArgs) {
        err = "ERR Protocol error: invalid multibulk length";
        return ParseResult::Error;
    }
    if (n <= 0) {                                 // *0 or *-1: valid, no command
        consumed = pos;
        return ParseResult::Ok;
    }

    args.reserve(static_cast<size_t>(n));

    for (long long i = 0; i < n; i++) {
        if (!read_line(buf, pos, line)) { args.clear(); return ParseResult::NeedMore; }

        if (line.empty() || line[0] != '$') {
            err = "ERR Protocol error: expected '$', got '" +
                  (line.empty() ? std::string() : std::string(1, line[0])) + "'";
            return ParseResult::Error;
        }

        long long len;
        if (!to_ll(line.substr(1), len) || len < 0 || len > kMaxBulkLen) {
            err = "ERR Protocol error: invalid bulk length";
            return ParseResult::Error;
        }

        // Need the payload plus its trailing CRLF. If any of it is missing,
        // throw away what we have and wait -- a command is all or nothing.
        if (buf.size() < pos + static_cast<size_t>(len) + 2) {
            args.clear();
            return ParseResult::NeedMore;
        }

        // Copy exactly len bytes rather than scanning for a delimiter, so a
        // payload can contain \r\n or NUL.
        args.emplace_back(buf, pos, static_cast<size_t>(len));
        pos += static_cast<size_t>(len) + 2;
    }

    consumed = pos;
    return ParseResult::Ok;
}

// Simple strings and errors end at the first CRLF, so they are not binary safe
// -- never put client-supplied text in them. Bulk strings are length-prefixed.
std::string reply_simple(const std::string& s)  { return "+" + s + "\r\n"; }
std::string reply_error(const std::string& s)   { return "-" + s + "\r\n"; }
std::string reply_integer(long long n)          { return ":" + std::to_string(n) + "\r\n"; }
std::string reply_nil()                         { return "$-1\r\n"; }
std::string reply_empty_array()                 { return "*0\r\n"; }

std::string reply_bulk(const std::string& s) {
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}
