#!/usr/bin/env python3
"""
A deliberately unreliable S3 front end.

Sits between S3Reader and the real (mock) S3 endpoint and damages the HTTP
exchange on purpose, so phase 2 tests the TRANSPORT rather than a decorator
inside our own reader. The v1 FaultyObjectReader proves the contract refuses a
short read; this proves that a genuinely truncated HTTP response is what the
contract sees.

  s3-fault-proxy.py --port 5556 --upstream 127.0.0.1:5555 --control FILE

The mode is read from the control file on every request, so a test changes
behaviour without restarting anything. Writing the file resets the per-mode
counters, which is what makes "fail the first attempt" mean the same thing
every run.

Modes:
  ok                    pass through
  fail_first:N          first N exchanges answer 503, then pass through
  truncate_first:N      first N exchanges send half the body and close, with
                        Content-Length still claiming the full length
  truncate_get:N        the same, keyed on the GET ordinal so the HEAD that
                        precedes a scan does not absorb it
  fail_get_first:N      first N GETs answer 503, then pass through
  fail_get_after:N      GETs after the Nth answer 500 permanently -- a failure
                        arriving MID-SCAN, after rows have been decoded
  truncate_all          every GET does that
  permanent             every exchange answers 500
  short_object:N        HEAD reports N bytes; GETs still serve the real object,
                        so the reader is told the object is shorter than it is
  ignore_range          GETs answer 200 with the whole object
  no_length             GETs answer 206 with no Content-Length
  hang                  accept, promise a body, send nothing, never close
  hang_get:N            the same for GETs after the Nth -- a stall arriving
                        mid-scan on whatever thread Arrow is reading from
  fail_then_replace:N   fail GET N transiently AND replace the object before
                        the retry arrives -- the retry must still carry the
                        identity captured at open and be refused

Deterministic by construction: every trigger is an exchange ordinal, never a
clock or a random number.
"""
import argparse
import http.client
import os
import socket
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Re-signing is needed only in front of a server that VERIFIES SigV4. The
# client signs for this proxy's host; the upstream checks the signature over
# ITS host, so forwarding the client's Authorization unchanged always fails.
# The proxy therefore strips it and re-signs with the same credentials, which
# is fine for a test harness and is the only way the fault modes can be used
# against a real server. Without botocore the proxy still works for an
# endpoint that does not check auth.
try:
    from botocore.auth import SigV4Auth
    from botocore.awsrequest import AWSRequest
    from botocore.credentials import Credentials
    _HAVE_BOTOCORE = True
except ImportError:
    _HAVE_BOTOCORE = False

STATE = {"mode": "ok", "mtime": None, "n": 0, "g": 0}
LOCK = threading.Lock()


def current_mode(path):
    """Re-read the control file; a change resets the exchange counter."""
    try:
        st = os.stat(path)
    except OSError:
        return "ok"
    with LOCK:
        if STATE["mtime"] != st.st_mtime:
            try:
                with open(path) as f:
                    STATE["mode"] = f.read().strip() or "ok"
            except OSError:
                STATE["mode"] = "ok"
            STATE["mtime"] = st.st_mtime
            STATE["n"] = 0
            STATE["g"] = 0
        return STATE["mode"]


def next_ordinal(is_get):
    """Two counters, because they answer different questions.

    `n` counts every exchange and is what fail_first uses -- "the first thing
    that happens", whatever it is. `g` counts only GETs, because a mode aimed
    at a DATA read must not be consumed by the HEAD that precedes it. Modes
    keyed on `n` silently landed on the HEAD and the GET came through clean,
    which made two tests pass while injecting nothing.
    """
    with LOCK:
        STATE["n"] += 1
        if is_get:
            STATE["g"] += 1
        return STATE["n"], STATE["g"]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "xpb-fault-proxy"

    def log_message(self, *a):            # quiet; the test reads counters
        pass

    # -- plumbing ----------------------------------------------------------
    def _upstream(self, method, path=None):
        host, _, port = self.server.upstream.partition(":")
        path = path or self.path
        c = http.client.HTTPConnection(host, int(port), timeout=60)

        if self.server.resign:
            # Forward the semantics (Range, If-Match) and sign them afresh for
            # the upstream host. Both are signed headers there too, so they
            # must be present before signing, not added after.
            hdrs = {"host": self.server.upstream,
                    "x-amz-content-sha256":
                        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}
            for k in ("Range", "If-Match", "If-None-Match"):
                v = self.headers.get(k)
                if v is not None:
                    hdrs[k.lower()] = v
            req = AWSRequest(method=method,
                             url="http://" + self.server.upstream + path,
                             headers=hdrs)
            SigV4Auth(Credentials(self.server.access_key, self.server.secret_key),
                      "s3", self.server.region).add_auth(req)
            headers = dict(req.headers)
        else:
            headers = {}
            for k in ("Range", "If-Match", "If-None-Match", "Authorization",
                      "x-amz-content-sha256", "x-amz-date"):
                v = self.headers.get(k)
                if v is not None:
                    headers[k] = v
            headers["Host"] = self.server.upstream

        c.request(method, path, headers=headers)
        r = c.getresponse()
        body = r.read()
        return r, body

    def _send(self, status, headers, body, truncate_to=None):
        self.send_response(status)
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        if body is None:
            return
        if truncate_to is None:
            self.wfile.write(body)
            return
        # Content-Length already promised the full length; deliver less and
        # drop the connection. That is a cut transfer, not a short object.
        self.wfile.write(body[:truncate_to])
        self.wfile.flush()
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.close_connection = True

    def _error(self, status, why):
        payload = ("<?xml version=\"1.0\"?><Error><Code>Injected</Code>"
                   f"<Message>{why}</Message></Error>").encode()
        self._send(status, [("Content-Type", "application/xml"),
                            ("Content-Length", str(len(payload))),
                            ("Connection", "close")], payload)
        self.close_connection = True

    # -- the faults --------------------------------------------------------
    def _handle(self, method):
        mode = current_mode(self.server.control)
        n, g = next_ordinal(method == "GET")
        name, _, arg = mode.partition(":")
        if self.server.trace:
            print(f"{method} n={n} g={g} mode={mode} range={self.headers.get('Range')} "
                  f"if-match={self.headers.get('If-Match')}", flush=True)

        if name == "permanent":
            return self._error(500, "permanent injected failure")

        if name == "hang" or (name == "hang_get" and method == "GET"
                              and g > int(arg or 0)):
            # Accept the connection, promise a body, send nothing, never close.
            # The nastiest shape for a client with no socket timeout: the
            # exchange neither succeeds nor fails.
            self.send_response(206)
            self.send_header("Content-Length", "1048576")
            self.send_header("Content-Range", "bytes 0-1048575/180143345")
            self.end_headers()
            try:
                import time as _t
                while True:
                    _t.sleep(5)
            except Exception:       # noqa: BLE001
                pass
            return

        if name == "fail_first" and n <= int(arg or 1):
            return self._error(503, f"injected transient failure {n}")

        # Keyed on the GET ordinal, so the HEAD never absorbs them.
        if name == "fail_get_first" and method == "GET" and g <= int(arg or 1):
            return self._error(503, f"injected transient GET failure {g}")

        if name == "fail_then_replace" and method == "GET" and g == int(arg or 1):
            # THE PHASE 6 CASE. Fail this GET transiently AND replace the
            # object before the client's retry arrives, so the retry meets a
            # different incarnation. A reader that re-HEADed on retry would
            # silently adopt the new one.
            try:
                _replace_object(self.server)
                why = "injected transient failure, object replaced underneath"
            except Exception as e:                      # noqa: BLE001
                why = f"injected transient failure, replacement FAILED: {e}"
            return self._error(503, why)

        if name == "fail_get_after" and method == "GET" and g > int(arg or 0):
            # A permanent failure arriving MID-SCAN, after the footer was read
            # and some row groups already decoded. The query must fail rather
            # than return the rows it happened to get.
            return self._error(500, f"injected permanent failure at GET {g}")

        if name == "short_object" and method == "HEAD":
            r, _ = self._upstream(method)
            return self._send(r.status,
                              [("Content-Length", str(int(arg or 0))),
                               ("Accept-Ranges", "bytes"),
                               ("Connection", "close")]
                              + _identity_headers(r), None)

        try:
            r, body = self._upstream(method)
        except Exception as e:                      # noqa: BLE001
            return self._error(502, f"upstream: {e}")

        if method == "HEAD":
            return self._send(r.status,
                              [("Content-Length", r.getheader("Content-Length", "0")),
                               ("Accept-Ranges", "bytes"),
                               ("Connection", "close")]
                              + _identity_headers(r), None)

        if name == "ignore_range":
            # Answer 200 with the whole object, which is what a misconfigured
            # endpoint does. The reader must refuse rather than slice it.
            saved = self.headers
            class _NoRange:
                def __init__(self, h): self._h = h
                def get(self, k, d=None):
                    return d if k in ("Range",) else self._h.get(k, d)
            self.headers = _NoRange(saved)
            try:
                whole, full = self._upstream("GET")
            finally:
                self.headers = saved
            return self._send(200, [("Content-Length", str(len(full))),
                                    ("Connection", "close")], full)

        hdrs = [("Connection", "close")] + _identity_headers(r)
        cr = r.getheader("Content-Range")
        if cr:
            hdrs.append(("Content-Range", cr))

        if name == "no_length":
            return self._send(r.status, hdrs, body)

        hdrs.append(("Content-Length", str(len(body))))

        if (name == "truncate_all" or
                (name == "truncate_first" and n <= int(arg or 1)) or
                (name == "truncate_get" and g <= int(arg or 1))):
            cut = len(body) // 2
            if cut == len(body):
                cut = max(0, len(body) - 1)
            return self._send(r.status, hdrs, body, truncate_to=cut)

        return self._send(r.status, hdrs, body)

    def do_GET(self):
        self._handle("GET")

    def do_HEAD(self):
        self._handle("HEAD")


def _identity_headers(r):
    """ETag and Last-Modified, forwarded verbatim.

    Dropping ETag makes a reader with an identity guard refuse to open, which
    is how this was found: the guard reported "the server returned no ETag"
    and stopped. Forwarding it is not cosmetic -- it is what makes the proxy a
    transparent stand-in for the real server.
    """
    out = []
    for h in ("ETag", "Last-Modified"):
        v = r.getheader(h)
        if v is not None:
            out.append((h, v))
    return out


def _replace_object(srv):
    """Overwrite the object under test with the staged replacement.

    Server-side copy, so it is one fast operation rather than a 180 MB upload
    racing the scan.
    """
    import boto3
    from botocore.config import Config
    s3 = boto3.client("s3", endpoint_url="http://" + srv.upstream,
                      aws_access_key_id=srv.access_key,
                      aws_secret_access_key=srv.secret_key,
                      region_name=srv.region,
                      config=Config(s3={"addressing_style": "path"}))
    bucket, _, key = srv.replace_target.partition("/")
    s3.copy_object(Bucket=bucket, Key=key,
                   CopySource={"Bucket": bucket, "Key": srv.replace_source})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=5556)
    ap.add_argument("--upstream", default="127.0.0.1:5555")
    ap.add_argument("--control", required=True)
    ap.add_argument("--trace", action="store_true",
                    help="log every request's mode decision, for diagnosing a "
                         "fault that did not fire")
    ap.add_argument("--resign", action="store_true",
                    help="re-sign upstream requests; required in front of a "
                         "server that verifies SigV4")
    ap.add_argument("--access-key", default="rustkey")
    ap.add_argument("--secret-key", default="rustsecret0123456789")
    ap.add_argument("--region", default="us-east-1")
    ap.add_argument("--replace-target", default="xpb/reg_buh.parquet")
    ap.add_argument("--replace-source", default="staged_B.parquet")
    a = ap.parse_args()

    if not os.path.exists(a.control):
        with open(a.control, "w") as f:
            f.write("ok\n")

    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    srv.upstream = a.upstream
    srv.control = a.control
    srv.resign = a.resign
    srv.trace = a.trace
    srv.access_key = a.access_key
    srv.secret_key = a.secret_key
    srv.region = a.region
    srv.replace_target = a.replace_target
    srv.replace_source = a.replace_source
    srv.daemon_threads = True
    if a.resign and not _HAVE_BOTOCORE:
        print("!! --resign needs botocore", flush=True)
        return 2
    print(f"fault proxy on 127.0.0.1:{a.port} -> {a.upstream}, control {a.control}",
          flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
