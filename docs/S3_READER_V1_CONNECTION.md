# S3Reader v1 — a persistent HTTP connection

Base commit `a6f5bf9`. Branch `objectreader-v2` (continued).

## Question

Can the cost of a new TCP connection per range GET be removed without changing
ObjectReader semantics, retry, the identity guard or cancellation?

## Answer

Yes, and the mechanism is small. **402 range GETs now travel on 1 connection
instead of 403.** Nothing else about the scan changed: same logical requests,
same bytes, same checksums, same projection and pruning counts, same failure
classifications, same identity behaviour.

The honest half of the answer is in [What it is worth](#what-it-is-worth): on
this loopback endpoint the saving is **0.04 % of a full scan**, which no wall
clock here can resolve. The mechanism is right; the benefit is proportional to
what a connection costs, and here that is 8 µs.

## The measurement that chose the design

Taken before writing anything, because "is one serialised connection enough"
is a question about Arrow's scheduling and not about HTTP. `max_in_flight` is
the most requests ever inside `request()` at once, counted before the lock:

| shape | GET | attempts | connections (before) | max in flight |
|---|---|---|---|---|
| footer only | 2 | 3 | 3 | **1** |
| one row group | 6 | 7 | 7 | **1** |
| pruned, 4 cols | 44 | 45 | 45 | **1** |
| full scan, 4 cols | 402 | 403 | 403 | **1** |

**Reads never overlap**, even though every column-chunk read runs on an Arrow
worker thread rather than the backend's: `pre_buffer` dispatches to the pool and
then waits for each range. So one socket under a mutex serialises nothing that
was concurrent, and the lock is uncontended by construction rather than by luck.

The mutex is held anyway, for two reasons that survive the measurement: the
socket and its HTTP state are shared mutable state that an Arrow worker and the
backend thread both touch, and a correctness argument that rests on Arrow's
scheduler staying what it is today is not a correctness argument.

The counter is kept in the code for the same reason it was written: without it,
"the mutex is free" is an assertion about Arrow rather than a measurement of it.

## Mechanism

`request()` was split in two. `do_exchange()` performs one HTTP exchange on a
descriptor it does not own — it never connects and never closes — and reports
three things: the status, whether any response byte arrived, and whether the
socket is still at a clean message boundary. `request()` owns the connection and
decides. The response parsing moved unchanged, so no failure classification
moved with it.

**The socket is dropped, not reused**, whenever it is not known to be at a
boundary:

* the response said `Connection: close` — parsed as a comma-separated token
  list, because a proxy may legitimately send `close, foo` and an equality test
  would miss it;
* the body did not match `Content-Length` exactly. Short is the cut-transfer
  case; longer means the server sent something this code does not model. Either
  way the next response could not be located reliably;
* the exchange failed at all.

`Connection` is hop-by-hop and not in the signed header set, so switching it
changes nothing about SigV4. Verified rather than assumed: the Authorization
oracle in section 0 of `s3_reader.sh` still matches botocore byte for byte.

## Reconnect

A reused socket has exactly one failure a fresh one does not: the server closed
it while it sat idle, and the client finds out by sending into a dead
connection. One request is replayed for that, on a new socket, under conditions
that are deliberately narrow:

* only on a socket that was **reused**, never a fresh one;
* only when **no response byte had arrived**, so a replay can never sit on top
  of a partly delivered answer;
* GET and HEAD only, which is all this reader issues, and both are idempotent.

It is counted as a **reconnect**, not a retry, and spends no retry budget —
there was no server answer to retry. `s3_reconnects()` is separate from
`s3_retries()` because they are different events.

### Why the narrowness is the whole of it

A replay that could happen after bytes arrived would hide a truncated transfer,
which is the one thing the v1 contract exists to surface. That is a test, not an
argument — case E below.

### Identity across a reconnect

The replay re-signs (new `x-amz-date`, new signature) and carries the **same**
`If-Match`, because `build_request()` rebuilds it from the token pinned at open
and `size()` is called once for the reader's life. A reconnect cannot re-HEAD,
cannot observe a new ETag and cannot widen the incarnation a reader reads. The
ObjectReader v2 invariant is therefore untouched by construction, and re-verified
by the existing replacement tests, which still produce 412 and a refusal.

## What was tested, and how it can fail

`extension/xpb_parquet/test/s3_keepalive_unit.cpp`, driven from section 9 of
`s3_reader.sh`. Every case reads the **same 17 ranges and compares each one
against the local copy of the object**, so "the connection was reused" is never
the only thing asserted — a reader that reused a socket and returned the wrong
half of the object would fail these.

| case | setup | result |
|---|---|---|
| A | reuse on, healthy endpoint | 17 GETs on **1** connection, bytes correct, 0 reconnects |
| B | `XPB_S3_CONNECTION_REUSE=off` | **18** connections for 18 requests, same bytes |
| C | a transparent TCP relay in between | still 1 connection, same bytes |
| D | relay kills every connection on its 2nd request | bytes correct, **17 reconnects, 0 retries**, 18 connections |
| E | relay cuts a response after 400 bytes | `truncated response, not EOF`, and **reconnects unchanged** |

Case D is the reconnect path proven to work; **case E is the proof it does not
fire when it must not.** Both triggers are request ordinals, never clocks: an
idle-timeout test that depends on when a timer fires is a test that passes for
the wrong reason, which this project has already paid for once.

Through SQL, section 9 asserts against whichever endpoint the server is
configured with, and both branches are real assertions:

* an endpoint that persists → connections must be far below the request count;
* an endpoint that answers `Connection: close` → connections must **equal** the
  request count **and** reconnects must be 0, because a reconnect there would
  mean the reader tried to reuse a socket the peer had already closed.

The fault proxy is the second case (it closes every response), so both branches
are exercised in a normal run: 403 connections for 403 requests with 0
reconnects through the proxy, 1 connection for 403 requests against RustFS.

## What it is worth

**On this endpoint: nothing measurable, and that is the result.**

One TCP connection to the endpoint, timed in isolation with the same syscall
sequence `connect_endpoint()` performs:

```
n=500  min=0.005 ms  median=0.008 ms  p95=0.018 ms  max=1.635 ms
```

402 of them is **3.2 ms**, or **0.04 %** of an 8.8 s full scan. The wall clock
agrees that there is nothing to see:

| full scan, 4 cols | runs |
|---|---|
| one connection per request | 9275, 8835, 8817, 8785, 8734 ms |
| keep-alive | 8681, 8785, 8552, 8891, 8811 ms |

Medians 8817 vs 8785 ms. The ranges overlap, the difference is smaller than the
run-to-run spread, and the effect being looked for is 3 ms. **No performance
claim is made from this table**; it is here because its shape is the honest
answer to "measure the full-scan effect".

### Where the handshake does cost something

`sch_netem` is absent from this kernel (`tc` answers *Specified qdisc kind is
unknown*), so network delay cannot be added at the network layer. Instead
`s3-latency-relay.py` charges a fixed cost **per connection** and nothing else —
once, after accept, never again. That isolates exactly the term keep-alive
removes; request bytes, server time and body transfer are identical in both
modes, so the comparison measures connection reuse and not a simulated network.
It is **not** a WAN simulation: a real high-latency endpoint also pays round
trips inside each exchange, in both modes, and those cancel in a before/after on
reuse.

At 5 ms per connection:

| shape | per-request connection | keep-alive | predicted saving | measured |
|---|---|---|---|---|
| footer, 3 requests | 65.8 ms | 51.9 ms | 10 ms | 13.9 ms |
| one row group, 7 | 180.6 ms | 136.7 ms | 30 ms | 43.9 ms |
| full scan, 403 | 11146, 11122, 10979 ms | 8803, 8619, 8661, 8834 ms | 2010 ms | ~2340 ms |

The full-scan ranges no longer overlap. The measured saving exceeds the
arithmetic by ~15 %, which is the relay's own per-connection cost (402 extra
Python threads and sockets) and not an extra win; the arithmetic is the number
to trust.

**So the result generalises as arithmetic, not as a benchmark:** the saving is
`(connections_before - connections_after) x cost_of_a_connection`. On loopback
that is 3 ms. At a 5 ms RTT it is 2 s on this scan. Against a TLS endpoint it
would be larger still, and TLS is out of scope here.

## What did not change

Checked rather than assumed, each with the evidence that would have caught the
opposite:

* **Logical requests and bytes.** 402/44/23/2 GETs for the four shapes, 52 795 301
  bytes for the full scan — the same figures every earlier phase measured.
* **Answers.** S3, local file and the PostgreSQL heap agree on the group-by
  checksum; benchmark 08 reports 14/0 and 41/0.
* **Failure semantics.** Transient 503 retried, cut transfer retried and paid
  for, unrecoverable cut reported as *not EOF*, mid-scan permanent failure fails
  the query. Note that the fault proxy answers `Connection: close`, so these run
  on fresh connections — which is why case E exists, to reach a cut transfer on
  a **reused** socket.
* **Deadlines.** A stalled endpoint still returns control in 46 s with
  `max_attempts=3`, bounded by the transport's own `SO_RCVTIMEO` rather than by
  an interrupt. A reused socket inherits the deadline set at connect.
* **Cancellation.** `pg_cancel_backend` and `pg_terminate_backend` still end a
  mid-remote-scan backend in under 500 ms. This needed checking: `close()` now
  takes the connection mutex, so if a worker thread were still inside an
  exchange, close would wait for it — up to one I/O deadline. The measurement
  says it does not arise, because Arrow's pool is shut down before the reader is
  destroyed.
* **No PostgreSQL symbol is named in the transport**, so nothing can raise from
  a worker thread; the interrupt hook still runs in the caller's retry loop,
  **outside** the mutex. That ordering is deliberate: the hook may longjmp out of
  a PostgreSQL ERROR, and a longjmp through a held `std::mutex` would leave the
  reader locked for good.
* **Nothing above ObjectReader changed.** No XPBatch source file differs; the
  Arrow adapter, `xpq_read_row_group` and the stats reader are untouched.

## Known limits

* **The benefit is zero where a connection is cheap.** Stated as a property of
  the endpoint, not hidden: loopback gains 3 ms on an 8.8 s scan.
* **No connection-health probe.** A server that keeps the socket open but stops
  answering costs one I/O deadline before the reader gives up on it. Catching
  that earlier needs either a probe or a shorter first-byte deadline, and
  neither has a measured need yet.
* **One connection per reader, not per backend.** Two concurrent scans of two
  objects hold two connections, and a reader opened and closed per query pays
  one handshake per query. A pool would address that and is explicitly out of
  scope.
* **No HTTP/2, no pipelining, no async, no prefetch.** Out of scope by the
  task, and the `max_in_flight = 1` measurement says pipelining would have
  nothing to pipeline at present.
* **The counters that are not atomic.** `http_attempts_`, `bytes_transferred_`
  and `reconnects_` are written only under the connection mutex, and
  `get_calls_`, `retries_done_` and `http_errors_` are written in `read_at_most`
  outside it, from whichever thread Arrow used. They are read on the backend
  thread after the scan, and Arrow's own join plus the mutex supply the
  happens-before edge. This is unchanged from v0 for the second group and
  improved for the first; widening the lock to cover `read_at_most` would put
  the interrupt hook inside it, which is the longjmp hazard above. Recorded as
  an accepted narrow boundary, with `max_in_flight = 1` as the reason it is
  narrow.

## Architecture verdict

**A — connection reuse is a transport-internal optimisation, and it stayed
inside the transport.**

* `ObjectReader` gained nothing: no API, no field, no notion that a connection
  exists. `ParquetReader`, Arrow and the XPBatch operators are unchanged.
* Retry, the identity guard, the deadlines and the accounting are untouched, and
  each is re-verified rather than assumed.
* The one new failure mode a persistent connection introduces is handled by a
  single bounded replay whose conditions are narrow enough to be stated in one
  sentence, and whose *non*-firing is tested.
* The correctness evidence is byte comparison against the object, not connection
  counts.

Not B: no escape hatch was needed anywhere, and nothing above the transport
learned anything. Not C: the change is 309 lines in one file, of which the
mechanism is a split function, a mutex, a descriptor and a two-pass loop.

**What this phase does not establish** is that connection reuse is worth having
on the endpoints this project will actually use, because there are none here
with a real network cost. It establishes that the mechanism is free of
correctness cost and that the saving is arithmetic in the cost of a connection.

## Regression

| suite | result |
|---|---|
| `s3_reader.sh` against RustFS, with the new section 9 | **81 correct, 0 wrong** |
| `object_reader.sh` (local path) | **57 correct, 0 wrong** |
| benchmark 08 | 14 agree / 0 differ, and 41 / 0 for the provider |
| `s3_keepalive_unit` standalone | 10 ok, 0 wrong |

### Two test defects found, and they matter more than the mechanism

**1. `same()` in section 6 called two identical errors "identical".** Eight
comparisons reported *identical* with the compared value being the same
PostgreSQL error text, because a concurrently redeclared `xpq_scan` made every
scan fail. The function could not tell *both right* from *both broken*. It now
requires each arm to have produced an answer — non-empty and not an error —
before equality means anything, and the same guard was added to the three-way
group-by comparison. Verified by driving the real function with a stub that
returns an error, an empty string, equal answers and unequal answers: 1 ok and
3 wrong, as it should be.

**2. The relay's request counter only advanced in one mode.** The
response-cutting mode keys on the same ordinal as the kill mode, and the counter
was incremented only under the kill mode, so the ordinal stayed at zero, the cut
never fired, and case E passed while testing nothing. Caught because case E
reported `ok=1` with an empty error — a cut transfer that produced no error at
all is not a plausible pass.

Two stale `xpq_scan` declarations were also brought up to date
(`object_reader.sh`, `benchmarks/08-parquet-source/check.py`); both predate this
phase by two columns. There are now three independent declarations of that
function, which is how they go stale; not consolidated here, because it is not
this phase's task.
