#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>

#include "resp.hpp"
#include "store.hpp"
#include "commands.hpp"

// Single-threaded epoll event loop. Every socket is non-blocking and the
// kernel reports which are ready, so one thread serves thousands of clients.
// Running one command at a time also means the Store needs no locks.

namespace {

constexpr int kDefaultPort = 6380;
constexpr int kMaxEvents   = 1024;      // events harvested per epoll_wait call
constexpr size_t kReadChunk = 16 * 1024;

// Without a cap, a client can announce a huge value and then send it slowly:
// nothing is ever parsed, so the buffer grows until we run out of memory.
constexpr size_t kMaxQueryBuf = 64 * 1024 * 1024;

// The same problem in the other direction: a client pipelines thousands of
// large reads and never reads the replies. Replies pile up in c.out because
// the socket will not take them. Measured at 818 MB from one connection, which
// also starved every other client. Drop such a client -- we cannot deliver to
// it anyway.
constexpr size_t kMaxOutputBuf = 64 * 1024 * 1024;

// The loop returns to the top after every event, so per-connection state has
// to live here rather than on the stack.
struct Conn {
    int fd = -1;
    std::string in;               // received bytes not yet parsed
    std::string out;              // replies not yet written to the socket
    bool close_after_flush = false;
    uint32_t events = 0;          // epoll mask currently registered
};

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// epoll_ctl is a syscall, so skip it when the mask has not changed.
void update_interest(int ep, Conn& c, uint32_t want) {
    if (c.events == want) return;
    epoll_event ev{};
    ev.events  = want;
    ev.data.fd = c.fd;
    if (epoll_ctl(ep, EPOLL_CTL_MOD, c.fd, &ev) == 0) c.events = want;
}

} // namespace

int main(int argc, char** argv) {
    int port = kDefaultPort;
    if (argc > 1) {
        port = std::atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "usage: %s [port]\n", argv[0]);
            return 1;
        }
    }

    // Writing to a dead socket raises SIGPIPE, which would kill the process.
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, nullptr, _IOLBF, 0);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(listen_fd, SOMAXCONN) < 0)                    { perror("listen"); return 1; }

    // Non-blocking too, since we accept in a loop until EAGAIN.
    if (set_nonblocking(listen_fd) < 0) { perror("fcntl"); return 1; }

    int ep = epoll_create1(0);
    if (ep < 0) { perror("epoll_create1"); return 1; }

    // The listening socket is just another fd in the set, so accepting becomes
    // an event like any other rather than something between clients.
    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = listen_fd;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, listen_fd, &ev) < 0) { perror("epoll_ctl"); return 1; }

    // Optional second argument caps the keyspace and enables LRU eviction.
    size_t max_keys = (argc > 2) ? static_cast<size_t>(std::atoll(argv[2])) : 0;
    Store store(max_keys);
    if (max_keys) printf("maxkeys=%zu (LRU eviction enabled)\n", max_keys);

    std::unordered_map<int, Conn> conns;
    std::vector<epoll_event> events(kMaxEvents);

    printf("kvstore listening on port %d (epoll event loop)\n", port);

    auto close_conn = [&](int fd) {
        epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        conns.erase(fd);
    };

    // Push as much of c.out as the kernel takes; false means the peer is gone.
    auto flush_out = [&](Conn& c) -> bool {
        while (!c.out.empty()) {
            ssize_t n = write(c.fd, c.out.data(), c.out.size());
            if (n > 0) { c.out.erase(0, static_cast<size_t>(n)); continue; }
            if (n < 0 && errno == EINTR)  continue;
            // Send buffer full: keep the rest and wait for EPOLLOUT.
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            return false;
        }
        return true;
    };

    // Giving epoll_wait a timeout makes it return even with no I/O, which is
    // the heartbeat for background work without needing a second thread.
    constexpr int kCronMs = 100;
    int64_t next_cron = now_ms();

    while (true) {
        int n = epoll_wait(ep, events.data(), kMaxEvents, kCronMs);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        // Reap expired keys nobody reads -- lazy expiry alone never sees them.
        if (now_ms() >= next_cron) {
            store.active_expire_cycle();
            next_cron = now_ms() + kCronMs;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            uint32_t re = events[i].events;

            if (fd == listen_fd) {
                // Loop until EAGAIN: one notification can cover many pending
                // connections, and accepting only one per wakeup throttles us.
                while (true) {
                    sockaddr_in cli{};
                    socklen_t len = sizeof(cli);
                    int cfd = accept4(listen_fd, (sockaddr*)&cli, &len, SOCK_NONBLOCK);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // queue drained
                        if (errno == EINTR) continue;
                        perror("accept4");
                        break;
                    }
                    // Disable Nagle so small replies are not delayed.
                    int one = 1;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

                    epoll_event cev{};
                    cev.events  = EPOLLIN | EPOLLRDHUP;
                    cev.data.fd = cfd;
                    if (epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &cev) < 0) { close(cfd); continue; }

                    Conn& c = conns[cfd];
                    c.fd = cfd;
                    c.events = cev.events;
                }
                continue;
            }

            auto it = conns.find(fd);
            if (it == conns.end()) { epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr); close(fd); continue; }
            Conn& c = it->second;

            if (re & (EPOLLERR | EPOLLHUP)) { close_conn(fd); continue; }

            if (re & EPOLLIN) {
                bool dead = false;
                char chunk[kReadChunk];
                while (true) {
                    ssize_t r = read(fd, chunk, sizeof(chunk));
                    if (r > 0) { c.in.append(chunk, static_cast<size_t>(r)); continue; }
                    if (r == 0) { dead = true; break; }              // peer closed
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // drained
                    dead = true; break;
                }

                if (c.in.size() > kMaxQueryBuf) {
                    c.out += reply_error("ERR Protocol error: query buffer limit exceeded");
                    c.in.clear();
                    c.close_after_flush = true;
                }

                // Drain every complete command sitting in the buffer.
                bool over_output_limit = false;
                while (!c.close_after_flush) {
                    std::vector<std::string> args;
                    size_t consumed = 0;
                    std::string err;
                    ParseResult r = parse_command(c.in, args, consumed, err);
                    if (r == ParseResult::NeedMore) break;
                    if (r == ParseResult::Error) {
                        c.out += reply_error(err);
                        c.close_after_flush = true;   // stream is desynced now
                        break;
                    }
                    c.in.erase(0, consumed);
                    if (args.empty()) continue;
                    if (args.size() == 1 && (args[0] == "QUIT" || args[0] == "quit")) {
                        c.out += reply_simple("OK");
                        c.close_after_flush = true;
                        break;
                    }
                    c.out += execute(store, args);

                    // Stop before the reply backlog eats the server.
                    if (c.out.size() > kMaxOutputBuf) { over_output_limit = true; break; }
                }

                if (over_output_limit) { close_conn(fd); continue; }

                if (!flush_out(c)) { close_conn(fd); continue; }
                if (dead && c.out.empty()) { close_conn(fd); continue; }
                if (dead) c.close_after_flush = true;
            }

            // Send buffer drained: finish a reply that stalled earlier.
            if (re & EPOLLOUT) {
                if (!flush_out(c)) { close_conn(fd); continue; }
            }

            if (c.close_after_flush && c.out.empty()) { close_conn(fd); continue; }

            // Only ask for EPOLLOUT while bytes are pending. An idle socket is
            // always writable, so leaving it armed spins the loop at 100% CPU.
            uint32_t want = EPOLLIN | EPOLLRDHUP | (c.out.empty() ? 0u : EPOLLOUT);
            update_interest(ep, c, want);
        }
    }

    close(listen_fd);
    close(ep);
    return 0;
}
