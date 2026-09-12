#pragma once
#include <string>
#include <cstdint>
#include <cstddef>
#include "store.hpp"

// Append-only log. Every write is recorded to disk so the dataset survives a
// restart, and the file is replayed on startup to rebuild the store.
//
// What gets written is the EFFECT of a command, not the command itself: after
// any write we record the affected key's resulting state as `SET key value`
// plus `PEXPIREAT key <deadline>`, or `DEL key` if it is gone. Redis records
// the command instead. Recording effects makes replay idempotent and free of
// ordering subtleties -- there is no question of whether INCR replays correctly
// when the key expired in between. The cost is a larger file for counters.
//
// Deadlines are absolute. Logging a relative TTL and replaying it an hour later
// would revive a key with a fresh lifetime instead of leaving it long expired.
class Aof {
public:
    enum class Sync {
        Always,    // fsync after every write: no data loss, slowest
        EverySec,  // fsync at most once a second: lose <=1s, Redis's default
        No,        // never fsync explicitly: fastest, lose whatever the OS holds
    };

    ~Aof();

    // Replay `path` into `store`. Safe to call when the file does not exist.
    // A partially written record at the end of the file is expected after a
    // crash: it is discarded and the file truncated to the last complete
    // record. `truncated` reports how many bytes were dropped.
    static bool load(const std::string& path, Store& store,
                     size_t& applied, size_t& truncated, std::string& err);

    // Open for appending. Call after load().
    bool open(const std::string& path, Sync policy, std::string& err);
    bool enabled() const { return fd_ >= 0; }

    // Record the current state of `key`. Buffered, not yet on disk.
    void log_key(Store& store, const std::string& key);

    // Push the buffer to the OS and fsync according to the policy. Called once
    // per event-loop iteration, before we block in epoll_wait.
    bool flush();

    // Replace the log with the shortest sequence that reproduces the current
    // dataset. Without this the file grows forever: a key incremented a million
    // times leaves a million records describing one integer.
    //
    // Redis forks a child so the parent keeps serving. This is synchronous --
    // simpler, and correct for a single-threaded server, at the cost of a pause
    // proportional to the keyspace.
    bool rewrite(const std::string& path, Store& store, std::string& err);

    void close();

    size_t bytes_written() const { return written_; }
    size_t fsyncs() const { return fsyncs_; }

private:
    bool do_fsync();

    int fd_ = -1;
    std::string buf_;
    Sync policy_ = Sync::EverySec;
    int64_t last_fsync_ms_ = 0;
    size_t written_ = 0;
    size_t fsyncs_ = 0;
    size_t last_rewrite_size_ = 0;

public:
    // Compact when the log has grown past a floor AND doubled since the last
    // compaction. The ratio stops us rewriting constantly on a big dataset.
    static constexpr size_t kRewriteMinBytes = 4 * 1024 * 1024;
    bool should_rewrite() const {
        return written_ >= kRewriteMinBytes &&
               written_ >= last_rewrite_size_ * 2;
    }
};

// Encode a command as a RESP array -- the same format clients speak, so the
// log can be replayed through the ordinary parser.
std::string encode_command(const std::vector<std::string>& args);
