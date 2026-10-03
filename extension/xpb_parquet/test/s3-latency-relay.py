#!/usr/bin/env python3
"""
A byte-for-byte TCP relay that charges a fixed cost PER CONNECTION.

    s3-latency-relay.py --listen 9100 --upstream 127.0.0.1:9000 --connect-delay-ms 5

Why this and not tc/netem: this kernel has no sch_netem (tc reports "Specified
qdisc kind is unknown"), so no delay can be added at the network layer here.

What it models, precisely: the ONE round trip a client pays before it can send
on a NEW connection. The delay is applied once, after accept, and never again,
so a reused connection pays it once for the whole scan while a fresh connection
pays it per request. That is exactly -- and only -- the term keep-alive removes;
everything else on the path (request bytes, server time, body transfer) is
untouched and identical in both modes, which is what makes the comparison a
measurement of connection reuse rather than of a simulated network.

It is NOT a WAN simulation. A real high-latency endpoint also pays RTTs inside
the exchange, in both modes. Those cancel in a before/after on reuse; this does
not pretend to reproduce them.

No HTTP awareness, no re-signing: the bytes are relayed unchanged, so the Host
header the client signed is the Host the server verifies.
"""
import argparse
import socket
import selectors
import threading
import time
import sys

def pump_down(src, dst, cut_at, cut_bytes, state):
    """
    Relay upstream -> client, cutting the response to the Nth request on this
    connection after `cut_bytes` bytes.

    This is the negative control for the replay above: the client HAS received
    response bytes, so a replay would be wrong -- it would hide a cut transfer,
    which is the one failure the v1 contract exists to surface. The reader must
    report it and leave it to the caller's retry, not reconnect silently.
    """
    sent = 0
    try:
        while True:
            b = src.recv(65536)
            if not b:
                break
            if cut_at and state["seen"] >= cut_at:
                room = cut_bytes - sent
                if room <= 0:
                    break
                if len(b) >= room:
                    dst.sendall(b[:room])
                    break
                sent += len(b)
            dst.sendall(b)
    except OSError:
        pass
    finally:
        for s in (src, dst):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                s.close()
            except OSError:
                pass


def pump(src, dst, kill_at=0, state=None):
    """
    Relay src -> dst. With kill_at = N, the Nth request seen on THIS connection
    is never forwarded and both sockets are closed instead.

    That reproduces, deterministically, the one race a persistent connection has
    that a fresh one does not: the server closed an idle socket, the client has
    not learned that yet, and its next request goes into a dead connection. The
    client sees the close with no response byte received. No timing is involved,
    unlike a real idle timeout, which is the point -- a test that depends on when
    a timer fires is a test that passes for the wrong reason.

    Requests are counted by their header terminator. GET and HEAD carry no body,
    so each "\r\n\r\n" is exactly one request, and this reader sends each
    request in one write and never pipelines, so a chunk holds at most one.
    """
    try:
        while True:
            b = src.recv(65536)
            if not b:
                break
            if state is not None:
                # Counted whichever fault mode is armed: the response-cut mode
                # keys on the same ordinal, and counting only under kill_at left
                # it permanently at zero, so the cut never fired and case E of
                # s3_keepalive_unit passed while testing nothing.
                state["seen"] += b.count(b"\r\n\r\n")
            if kill_at and state["seen"] >= kill_at:
                break               # drop this request and close, silently
            dst.sendall(b)
    except OSError:
        pass
    finally:
        for s in (src, dst):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                s.close()
            except OSError:
                pass

def serve(listen_port, up_host, up_port, delay_ms, counter, kill_at=0,
          cut_at=0, cut_bytes=0):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", listen_port))
    srv.listen(128)
    print("relay on 127.0.0.1:%d -> %s:%d, %g ms per connection, kill_at=%d, "
          "cut_at=%d/%d B"
          % (listen_port, up_host, up_port, delay_ms, kill_at, cut_at, cut_bytes),
          flush=True)
    while True:
        cli, _ = srv.accept()
        counter[0] += 1
        threading.Thread(target=handle,
                         args=(cli, up_host, up_port, delay_ms, kill_at,
                               cut_at, cut_bytes),
                         daemon=True).start()

def handle(cli, up_host, up_port, delay_ms, kill_at=0, cut_at=0, cut_bytes=0):
    # The charge is here: once, before anything is relayed.
    if delay_ms > 0:
        time.sleep(delay_ms / 1000.0)
    try:
        up = socket.create_connection((up_host, up_port), timeout=30)
    except OSError:
        cli.close()
        return
    cli.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    up.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    state = {"seen": 0}
    threading.Thread(target=pump, args=(cli, up, kill_at, state),
                     daemon=True).start()
    threading.Thread(target=pump_down, args=(up, cli, cut_at, cut_bytes, state),
                     daemon=True).start()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", type=int, default=9100)
    ap.add_argument("--upstream", default="127.0.0.1:9000")
    ap.add_argument("--connect-delay-ms", type=float, default=5.0)
    ap.add_argument("--cut-at-request", type=int, default=0,
                    help="truncate the response to the Nth request on each "
                         "connection (0 = never)")
    ap.add_argument("--cut-bytes", type=int, default=400,
                    help="bytes of that response to deliver before closing")
    ap.add_argument("--kill-at-request", type=int, default=0,
                    help="close each connection when its Nth request arrives, "
                         "without forwarding it (0 = never)")
    a = ap.parse_args()
    h, p = a.upstream.split(":")
    counter = [0]
    try:
        serve(a.listen, h, int(p), a.connect_delay_ms, counter,
              a.kill_at_request, a.cut_at_request, a.cut_bytes)
    except KeyboardInterrupt:
        print("connections accepted: %d" % counter[0], flush=True)
        sys.exit(0)

if __name__ == "__main__":
    main()
