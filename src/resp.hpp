#pragma once
#include <string>
#include <vector>
#include <cstddef>

// RESP2: commands arrive as an array of bulk strings.
//   SET foo bar  ->  *3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n
// Lengths are explicit, so payloads may contain \r\n or NUL.

enum class ParseResult {
    Ok,        // complete command; `consumed` bytes may be erased
    NeedMore,  // partial command; nothing consumed
    Error,     // protocol violation; reply `err` and close
};

// Parses one command from the front of `buf`. Does no I/O and never modifies
// `buf` -- the caller owns the buffer and decides when to erase.
ParseResult parse_command(const std::string& buf,
                          std::vector<std::string>& args,
                          size_t& consumed,
                          std::string& err);

std::string reply_simple(const std::string& s);   // +OK\r\n
std::string reply_error(const std::string& s);    // -ERR ...\r\n
std::string reply_integer(long long n);           // :42\r\n
std::string reply_bulk(const std::string& s);     // $5\r\nhello\r\n
std::string reply_nil();                          // $-1\r\n  (key absent)
std::string reply_empty_array();                  // *0\r\n
