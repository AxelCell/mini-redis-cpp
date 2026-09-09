#!/usr/bin/env python3
"""
Fixed-window rate limiter built on kvstore.

Demonstrates the standard Redis rate-limiting pattern using only INCR and
EXPIRE. This is what sits in front of essentially every production API.

  INCR   ratelimit:<user>:<window>     -> current count for this window
  EXPIRE ratelimit:<user>:<window> N   -> window self-destructs when it ends

Why INCR rather than GET-then-SET: the server executes commands one at a time
on a single thread, so the read-modify-write inside INCR cannot interleave with
another client. Doing it from the client side would lose updates -- two clients
both read 5, both write 6, and one request goes uncounted.

Run:
    ./server 6380 &
    python3 examples/ratelimit.py
"""

import socket
import sys
import time

HOST, PORT = "127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 6380

LIMIT       = 5    # requests allowed per window
WINDOW_SECS = 3    # window length


class Client:
    """Minimal RESP client -- no third-party libraries needed."""

    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.buf = b""

    def cmd(self, *args):
        out = f"*{len(args)}\r\n".encode()
        for a in args:
            b = str(a).encode()
            out += b"$" + str(len(b)).encode() + b"\r\n" + b + b"\r\n"
        self.sock.sendall(out)
        return self._read_reply()

    def _read_line(self):
        while b"\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("server closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def _read_reply(self):
        line = self._read_line()
        tag, body = line[:1], line[1:]
        if tag == b"+":
            return body.decode()
        if tag == b":":
            return int(body)
        if tag == b"-":
            raise RuntimeError(body.decode())
        if tag == b"$":
            n = int(body)
            if n == -1:
                return None                      # null bulk: key absent
            while len(self.buf) < n + 2:
                self.buf += self.sock.recv(4096)
            val, self.buf = self.buf[:n], self.buf[n + 2:]
            return val.decode()
        raise RuntimeError(f"unexpected reply tag {tag!r}")


def allow_request(c, user):
    """Return (allowed, count, ttl). Two commands, one round trip each."""
    window = int(time.time()) // WINDOW_SECS
    key = f"ratelimit:{user}:{window}"

    count = c.cmd("INCR", key)

    # Only set the expiry on the first request of a window. INCR preserves an
    # existing TTL, so re-arming it every time would be harmless here -- but on
    # a *sliding* implementation it would keep pushing the window out forever,
    # and the key would never expire.
    if count == 1:
        c.cmd("EXPIRE", key, WINDOW_SECS)

    ttl = c.cmd("TTL", key)
    return count <= LIMIT, count, ttl


def main():
    c = Client(HOST, PORT)
    print(f"connected to kvstore at {HOST}:{PORT}")
    print(f"policy: {LIMIT} requests per {WINDOW_SECS}s window\n")

    print("--- 8 requests in quick succession ---")
    for i in range(1, 9):
        ok, count, ttl = allow_request(c, "user42")
        status = "200 OK       " if ok else "429 RATE LIMITED"
        print(f"  request {i}: {status}  count={count}/{LIMIT}  window resets in {ttl}s")
        time.sleep(0.1)

    print(f"\n--- waiting {WINDOW_SECS + 1}s for the window to expire ---")
    time.sleep(WINDOW_SECS + 1)

    print("\n--- new window, counter reset itself via TTL ---")
    for i in range(1, 4):
        ok, count, ttl = allow_request(c, "user42")
        status = "200 OK       " if ok else "429 RATE LIMITED"
        print(f"  request {i}: {status}  count={count}/{LIMIT}  window resets in {ttl}s")
        time.sleep(0.1)

    print("\n--- a different user has an independent budget ---")
    for i in range(1, 3):
        ok, count, ttl = allow_request(c, "user99")
        status = "200 OK       " if ok else "429 RATE LIMITED"
        print(f"  user99 request {i}: {status}  count={count}/{LIMIT}")

    keys = c.cmd("DBSIZE")
    print(f"\nkeys currently held: {keys}")
    print("expired windows are reclaimed automatically by the active-expiry cron,")
    print("so the keyspace does not grow with traffic.")


if __name__ == "__main__":
    main()
