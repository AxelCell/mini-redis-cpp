#include "aof.hpp"
#include "resp.hpp"
#include "commands.hpp"

#include <cerrno>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

std::string encode_command(const std::vector<std::string>& args) {
    std::string out = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& a : args)
        out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
    return out;
}

Aof::~Aof() { close(); }

bool Aof::load(const std::string& path, Store& store,
               size_t& applied, size_t& truncated, std::string& err) {
    applied = 0;
    truncated = 0;
    err.clear();

    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) return true;          // nothing to recover from
        err = "cannot open " + path + ": " + std::strerror(errno);
        return false;
    }

    std::string data;
    char chunk[64 * 1024];
    while (true) {
        ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n > 0) { data.append(chunk, static_cast<size_t>(n)); continue; }
        if (n == 0) break;
        if (errno == EINTR) continue;
        err = "read failed: " + std::string(std::strerror(errno));
        ::close(fd);
        return false;
    }
    ::close(fd);

    // Replay through the ordinary parser and dispatcher -- the log holds the
    // same RESP the network speaks, so recovery needs no separate code path.
    size_t offset = 0;
    while (offset < data.size()) {
        // The log only ever contains RESP arrays. Anything else is damage:
        // the parser would happily read it as an inline command, which would
        // silently apply garbage instead of reporting it.
        if (data[offset] != '*') {
            truncated = data.size() - offset;
            break;
        }

        const std::string rest = data.substr(offset);
        std::vector<std::string> args;
        size_t consumed = 0;
        std::string perr;

        ParseResult r = parse_command(rest, args, consumed, perr);

        // A crash can leave the final record half written. That is expected,
        // not corruption: drop the tail and carry on with what is complete.
        if (r == ParseResult::NeedMore || r == ParseResult::Error) {
            truncated = data.size() - offset;
            break;
        }

        offset += consumed;
        if (args.empty()) continue;
        execute(store, args);
        applied++;
    }

    // Cut the torn tail off the file so it cannot confuse the next startup or
    // get interleaved with new appends.
    if (truncated > 0) {
        if (::truncate(path.c_str(), static_cast<off_t>(offset)) != 0) {
            err = "could not truncate torn tail: " + std::string(std::strerror(errno));
            return false;
        }
    }
    return true;
}

bool Aof::open(const std::string& path, Sync policy, std::string& err) {
    close();
    err.clear();
    policy_ = policy;

    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) {
        err = "cannot open " + path + " for append: " + std::strerror(errno);
        return false;
    }
    last_fsync_ms_ = now_ms();
    return true;
}

void Aof::log_key(Store& store, const std::string& key) {
    if (fd_ < 0) return;

    const std::string* v = store.get_mut(key);
    if (v == nullptr) {                                   // absent or expired
        buf_ += encode_command({"DEL", key});
        return;
    }
    buf_ += encode_command({"SET", key, *v});

    // SET clears any TTL, so a key with a deadline needs it restored. Absolute,
    // so replaying later lands on the same instant.
    const int64_t deadline = store.deadline_of(key);
    if (deadline != 0)
        buf_ += encode_command({"PEXPIREAT", key, std::to_string(deadline)});
}

bool Aof::do_fsync() {
    if (fd_ < 0) return true;
    if (::fsync(fd_) != 0 && errno != EINVAL) return false;   // EINVAL: not a real file
    fsyncs_++;
    last_fsync_ms_ = now_ms();
    return true;
}

bool Aof::flush() {
    if (fd_ < 0) return true;

    // write() can take fewer bytes than offered, so loop.
    size_t sent = 0;
    while (sent < buf_.size()) {
        ssize_t n = ::write(fd_, buf_.data() + sent, buf_.size() - sent);
        if (n > 0) { sent += static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        buf_.erase(0, sent);
        written_ += sent;
        return false;
    }
    written_ += sent;
    const bool had_data = sent > 0;
    buf_.clear();

    switch (policy_) {
        case Sync::Always:
            if (had_data) return do_fsync();
            break;
        case Sync::EverySec:
            if (now_ms() - last_fsync_ms_ >= 1000) return do_fsync();
            break;
        case Sync::No:
            break;
    }
    return true;
}

bool Aof::rewrite(const std::string& path, Store& store, std::string& err) {
    err.clear();
    const std::string tmp = path + ".tmp";

    int tfd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (tfd < 0) {
        err = "cannot create " + tmp + ": " + std::strerror(errno);
        return false;
    }

    std::string out;
    bool io_ok = true;
    auto spill = [&]() {
        size_t sent = 0;
        while (sent < out.size()) {
            ssize_t n = ::write(tfd, out.data() + sent, out.size() - sent);
            if (n > 0) { sent += static_cast<size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            io_ok = false;
            return;
        }
        out.clear();
    };

    store.for_each([&](const std::string& k, const std::string& v, int64_t deadline) {
        out += encode_command({"SET", k, v});
        if (deadline != 0)
            out += encode_command({"PEXPIREAT", k, std::to_string(deadline)});
        if (out.size() >= 256 * 1024) spill();
    });
    if (io_ok) spill();

    if (!io_ok) {
        err = "write failed during rewrite: " + std::string(std::strerror(errno));
        ::close(tfd);
        ::unlink(tmp.c_str());
        return false;
    }

    // fsync the new file BEFORE renaming. rename() is atomic, so the log is
    // either entirely the old one or entirely the new one -- never a mix.
    if (::fsync(tfd) != 0 && errno != EINVAL) {
        err = "fsync failed during rewrite: " + std::string(std::strerror(errno));
        ::close(tfd);
        ::unlink(tmp.c_str());
        return false;
    }
    ::close(tfd);

    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        err = "rename failed: " + std::string(std::strerror(errno));
        ::unlink(tmp.c_str());
        return false;
    }

    // Point our append fd at the new file.
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    buf_.clear();
    if (!open(path, policy_, err)) return false;

    struct stat st{};
    written_ = (::stat(path.c_str(), &st) == 0) ? static_cast<size_t>(st.st_size) : 0;
    last_rewrite_size_ = written_;
    return true;
}

void Aof::close() {
    if (fd_ < 0) return;
    flush();
    do_fsync();
    ::close(fd_);
    fd_ = -1;
}
