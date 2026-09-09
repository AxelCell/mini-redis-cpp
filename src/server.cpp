#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <string>
#include <vector>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>

#include "resp.hpp"
#include "store.hpp"
#include "commands.hpp"

namespace {

constexpr int kDefaultPort = 6380;

// write() may write FEWER bytes than asked when the socket send buffer fills.
// Ignoring the return value is a classic silent bug: replies truncate only
// under load, exactly when it is hardest to debug. (This is also the warning
// -Wunused-result was giving us in the Phase 1 echo server.)
bool write_all(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;      // interrupted by a signal: retry
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Serve one client until it disconnects or desyncs the protocol.
//
// `buf` lives ACROSS read() calls because TCP is a byte stream: one read() may
// return half a command, or five commands glued together. So the loop is
// read -> drain every complete command -> read again.
void serve_client(int conn_fd, Store& store) {
    std::string buf;
    char chunk[16 * 1024];

    while (true) {
        ssize_t n = read(conn_fd, chunk, sizeof(chunk));
        if (n == 0) break;                     // clean EOF: client closed
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("read");
            break;
        }
        buf.append(chunk, static_cast<size_t>(n));

        std::string out;
        bool done = false;

        // Drain: parse while whole commands remain buffered.
        while (true) {
            std::vector<std::string> args;
            size_t consumed = 0;
            std::string err;

            ParseResult r = parse_command(buf, args, consumed, err);
            if (r == ParseResult::NeedMore) break;          // wait for more bytes
            if (r == ParseResult::Error) {
                out += reply_error(err);
                done = true;                                // stream is desynced
                break;
            }

            buf.erase(0, consumed);            // only ever on Ok
            if (args.empty()) continue;        // blank line

            if (args.size() == 1 && (args[0] == "QUIT" || args[0] == "quit")) {
                out += reply_simple("OK");
                done = true;
                break;
            }
            out += execute(store, args);
        }

        // One write() per read(), not one per command. With pipelining that
        // turns N syscalls into 1 -- a real, measurable throughput win.
        if (!out.empty() && !write_all(conn_fd, out.data(), out.size())) break;
        if (done) break;
    }
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

    // If a client vanishes mid-reply, write() to the dead socket raises
    // SIGPIPE, which by default KILLS the process. A server must ignore it and
    // handle the EPIPE from write() instead.
    signal(SIGPIPE, SIG_IGN);

    // printf is fully buffered when stdout is a pipe rather than a terminal,
    // so redirected logs appear only at exit -- or never, if we are killed.
    setvbuf(stdout, nullptr, _IOLBF, 0);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    // Allows rebinding while old connections linger in TIME_WAIT. Note it does
    // NOT let two live servers share a port -- that would be SO_REUSEPORT.
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(server_fd, SOMAXCONN) < 0)                    { perror("listen"); return 1; }
    printf("kvstore listening on port %d\n", port);

    // One store shared by all connections. Lock-free today only because we
    // serve one client at a time; Phase 3's event loop keeps that invariant
    // while serving thousands at once -- which is why Redis needs no locks.
    Store store;

    while (true) {
        sockaddr_in client{};
        socklen_t len = sizeof(client);
        int conn_fd = accept(server_fd, (sockaddr*)&client, &len);
        if (conn_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));
        printf("client connected: %s:%d\n", ip, ntohs(client.sin_port));

        serve_client(conn_fd, store);

        close(conn_fd);
        printf("client disconnected\n");
    }
}
