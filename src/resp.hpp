#pragma once
#include <string>
#include <vector>
#include <cstddef>

// RESP2 -- the Redis wire protocol.
// Commands arrive as an array of bulk strings:
//   SET foo bar  ->  *3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n
// Lengths are explicit, so payloads are binary safe (may contain \r\n or NUL).

enum class ParseResult {
    Ok,        // one complete command parsed; `consumed` bytes may be erased
    NeedMore,  // partial command; nothing consumed, read() more and retry
    Error,     // protocol violation; reply `err` and close the connection
};

// Parses ONE command from the front of `buf`. Does no I/O and does not modify
// `buf` -- the caller owns the buffer. Keeping this pure is what lets Phase 3
// swap blocking reads for epoll without changing a line of the parser.
ParseResult parse_command(const std::string& buf,
                          std::vector<std::string>& args,
                          size_t& consumed,
                          std::string& err);

// --- Reply encoders ---
std::string reply_simple(const std::string& s);   // +OK\r\n
std::string reply_error(const std::string& s);    // -ERR ...\r\n
std::string reply_integer(long long n);           // :42\r\n
std::string reply_bulk(const std::string& s);     // $5\r\nhello\r\n
std::string reply_nil();                          // $-1\r\n  (key absent)
std::string reply_empty_array();                  // *0\r\n
