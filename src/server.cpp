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

// ---------------------------------------------------------------------------
// Phase 3: single-threaded epoll event loop.
//
// Phase 2 blocked in read() on one client, so 49 of 50 connections sat unserved
// in the kernel accept queue. Here nothing blocks: every socket is O_NONBLOCK
// and the kernel tells us which ones are ready.
//
// Note what did NOT change: resp.cpp, store.hpp and commands.cpp are untouched.
// The parser was written as a pure function over a buffer precisely so the I/O
// strategy could be replaced without touching protocol logic.
//
// One thread means no mutexes anywhere: only one command runs at a time, so the
// Store cannot race with itself. That is the same reason real Redis is
// single-threaded for command execution.
// ---------------------------------------------------------------------------

namespace {

constexpr int kDefaultPort = 6380;
constexpr int kMaxEvents   = 1024;      // events harvested per epoll_wait call
constexpr size_t kReadChunk = 16 * 1024;

// Per-connection state. In Phase 2 this lived in local variables inside
// serve_client(); the stack frame WAS the state. An event loop returns to the
// top after every event, so the state must outlive the function call.
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

// Only call epoll_ctl when the mask actually changes. epoll_ctl is a syscall,
// and at high connection counts calling it on every event is measurable waste.
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

    // Writing to a socket whose peer vanished raises SIGPIPE, which by default
    // KILLS the process. Ignore it and handle EPIPE from write() instead.
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

    // The LISTENING socket must be non-blocking too: we accept in a loop until
    // EAGAIN, and a blocking accept() on an empty queue would stall the loop.
    if (set_nonblocking(listen_fd) < 0) { perror("fcntl"); return 1; }

    int ep = epoll_create1(0);
    if (ep < 0) { perror("epoll_create1"); return 1; }

    // The listening socket is just another fd in the set. That is the key
    // structural change: accepting is now an event like any other, not
    // something that only happens between clients.
    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = listen_fd;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, listen_fd, &ev) < 0) { perror("epoll_ctl"); return 1; }

    Store store;
    std::unordered_map<int, Conn> conns;
    std::vector<epoll_event> events(kMaxEvents);

    printf("kvstore listening on port %d (epoll event loop)\n", port);

    auto close_conn = [&](int fd) {
        epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        conns.erase(fd);
    };

    // Push as much of c.out as the kernel will take. Returns false if the
    // connection died.
    auto flush_out = [&](Conn& c) -> bool {
        while (!c.out.empty()) {
            ssize_t n = write(c.fd, c.out.data(), c.out.size());
            if (n > 0) { c.out.erase(0, static_cast<size_t>(n)); continue; }
            if (n < 0 && errno == EINTR)  continue;
            // EAGAIN: kernel send buffer is full. We cannot wait -- keep the
            // remainder buffered and let epoll tell us when it drains.
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            return false;                       // EPIPE / ECONNRESET etc.
        }
        return true;
    };

    while (true) {
        // -1 = sleep indefinitely. The thread consumes no CPU while idle,
        // however many thousands of connections are registered.
        int n = epoll_wait(ep, events.data(), kMaxEvents, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            uint32_t re = events[i].events;

            // ---------------- new connections ----------------
            if (fd == listen_fd) {
                // Loop until EAGAIN: one readiness notification can cover MANY
                // pending connections. Accepting just one per wakeup is a
                // classic bug that quietly throttles connection throughput.
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
                    // Disable Nagle: we send small replies and do not want the
                    // kernel delaying them to coalesce with a later write.
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

            // ---------------- existing connections ----------------
            auto it = conns.find(fd);
            if (it == conns.end()) { epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr); close(fd); continue; }
            Conn& c = it->second;

            if (re & (EPOLLERR | EPOLLHUP)) { close_conn(fd); continue; }

            // --- readable ---
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

                // Drain every complete command out of the buffer. Identical to
                // Phase 2 -- the parser did not change.
                while (!c.close_after_flush) {
                    std::vector<std::string> args;
                    size_t consumed = 0;
                    std::string err;
                    ParseResult r = parse_command(c.in, args, consumed, err);
                    if (r == ParseResult::NeedMore) break;
                    if (r == ParseResult::Error) {
                        c.out += reply_error(err);
                        c.close_after_flush = true;                  // stream desynced
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
                }

                if (!flush_out(c)) { close_conn(fd); continue; }
                if (dead && c.out.empty()) { close_conn(fd); continue; }
                if (dead) c.close_after_flush = true;
            }

            // --- writable: the send buffer drained, finish a stalled reply ---
            if (re & EPOLLOUT) {
                if (!flush_out(c)) { close_conn(fd); continue; }
            }

            if (c.close_after_flush && c.out.empty()) { close_conn(fd); continue; }

            // Ask for EPOLLOUT only while we actually have pending bytes.
            // Leaving it armed on an idle writable socket would spin the loop
            // at 100% CPU -- the classic level-triggered busy-loop bug.
            uint32_t want = EPOLLIN | EPOLLRDHUP | (c.out.empty() ? 0u : EPOLLOUT);
            update_interest(ep, c, want);
        }
    }

    close(listen_fd);
    close(ep);
    return 0;
}
