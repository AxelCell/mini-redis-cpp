#include "resp.hpp"
#include <sstream>

namespace {

// --- Guard rails -----------------------------------------------------------
// A parser that trusts the network is a denial-of-service waiting to happen.
// Without these, a client sends "*999999999\r\n" and we reserve gigabytes, or
// dribbles bytes forever with no newline while our buffer grows unbounded.
constexpr size_t    kMaxInlineLen = 64 * 1024;            // 64 KB header/inline line
constexpr long long kMaxArgs      = 1024 * 1024;          // elements in one array
constexpr long long kMaxBulkLen   = 512LL * 1024 * 1024;  // 512 MB per argument

// Read one CRLF-terminated line at `pos`; advance `pos` past the CRLF.
//
// false means "not fully buffered YET" -- a NeedMore, not an error. Conflating
// a slow network with a broken client is the classic bug: it works on
// localhost and drops good connections under real latency.
bool read_line(const std::string& b, size_t& pos, std::string& line) {
    size_t nl = b.find("\r\n", pos);
    if (nl == std::string::npos) return false;
    line.assign(b, pos, nl - pos);
    pos = nl + 2;                       // step over both CR and LF
    return true;
}

// Strict integer parse: the ENTIRE string must be digits.
//
// Why not atoll()? atoll("12abc") silently returns 12, and atoll("abc")
// returns 0 -- indistinguishable from a real zero. A protocol parser must
// reject malformed input, never guess at it.
bool to_ll(const std::string& s, long long& out) {
    if (s.empty()) return false;
    size_t i = 0;
    bool neg = (s[0] == '-');
    if (neg || s[0] == '+') i = 1;
    if (i >= s.size()) return false;             // a lone "-" or "+"
    long long v = 0;
    for (; i < s.size(); i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
        if (v > kMaxBulkLen * 4) return false;   // stop before signed overflow (UB)
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
    consumed = 0;        // stays 0 unless we reach Ok -- the all-or-nothing rule
    err.clear();

    if (buf.empty()) return ParseResult::NeedMore;

    // ===================== INLINE COMMAND =====================
    // Anything not starting with '*' is a bare line: "SET foo bar\n".
    // That is what nc/telnet send. Real Redis accepts it too, and it keeps
    // manual testing possible without a RESP client.
    if (buf[0] != '*') {
        size_t nl = buf.find('\n');
        if (nl == std::string::npos) {
            if (buf.size() > kMaxInlineLen) {
                err = "ERR Protocol error: too big inline request";
                return ParseResult::Error;
            }
            return ParseResult::NeedMore;         // line not finished yet
        }
        std::string line(buf, 0, nl);
        if (!line.empty() && line.back() == '\r') line.pop_back();   // \r\n or \n
        consumed = nl + 1;

        std::istringstream iss(line);             // split on whitespace
        std::string word;
        while (iss >> word) args.push_back(word);
        return ParseResult::Ok;                   // args may be empty (blank line)
    }

    // ===================== RESP ARRAY =====================
    // *3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n
    //
    // `pos` is a CURSOR. We never modify buf, and `consumed` is published only
    // at the very end -- so every early return leaves the caller's buffer
    // exactly as it was.
    size_t pos = 0;
    std::string line;

    // --- array header: how many arguments? ---
    if (!read_line(buf, pos, line)) {
        if (buf.size() > kMaxInlineLen) {
            err = "ERR Protocol error: too big mbulk count string";
            return ParseResult::Error;
        }
        return ParseResult::NeedMore;
    }

    long long n;
    if (!to_ll(line.substr(1), n) || n > kMaxArgs) {   // substr(1) skips the '*'
        err = "ERR Protocol error: invalid multibulk length";
        return ParseResult::Error;
    }
    if (n <= 0) {                   // *0 (empty) or *-1 (null): valid, no command
        consumed = pos;
        return ParseResult::Ok;
    }

    args.reserve(static_cast<size_t>(n));

    // --- each element: $<len>\r\n<len bytes>\r\n ---
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

        // THE PARTIAL-READ CHECK.
        // We need `len` payload bytes PLUS the 2-byte trailing CRLF. If even
        // one is missing, discard what we parsed and wait. A command is atomic:
        // consume all of it, or none of it.
        if (buf.size() < pos + static_cast<size_t>(len) + 2) {
            args.clear();
            return ParseResult::NeedMore;
        }

        // Copy exactly `len` bytes -- never scan for a delimiter. That is why
        // a payload may contain \r\n or NUL. This is binary safety.
        args.emplace_back(buf, pos, static_cast<size_t>(len));
        pos += static_cast<size_t>(len) + 2;      // payload + CRLF
    }

    consumed = pos;      // only NOW does the caller learn it may erase anything
    return ParseResult::Ok;
}

// --- Reply encoders --------------------------------------------------------
// Simple strings and errors are DELIMITER-terminated: NOT binary safe, so
// never splice client data into them (CRLF injection). Bulk strings are
// LENGTH-prefixed and therefore safe for arbitrary bytes.

std::string reply_simple(const std::string& s)  { return "+" + s + "\r\n"; }
std::string reply_error(const std::string& s)   { return "-" + s + "\r\n"; }
std::string reply_integer(long long n)          { return ":" + std::to_string(n) + "\r\n"; }
std::string reply_nil()                         { return "$-1\r\n"; }   // absent
std::string reply_empty_array()                 { return "*0\r\n"; }

std::string reply_bulk(const std::string& s) {   // s.size() is a BYTE count
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}
