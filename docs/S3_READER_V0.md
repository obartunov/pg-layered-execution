# S3Reader v0 — a remote ObjectReader, in phases

Base commit `8876871`. Branch `s3-reader-v0`.

Eight phases with their own stop points, so transport, correctness and
performance do not get decided at once. What follows is per phase, then the
architectural verdict.

**What this cannot show, stated once and not softened later:** that the reader
works against real AWS S3. There is no TLS in it and no route to AWS from this
environment. Everything below runs against a local S3-compatible endpoint
(moto). SigV4 is therefore checked against botocore rather than against a
server, and the absolute timings are the mock's, not S3's.

---

## Phase 1 — transport skeleton

**Question:** does a remote reader sit under the existing `ObjectReader` with
nothing above it changing?

**Answer: yes, for the price of a three-line scheme branch.**

```
make_object_reader(uri)
    |
    +-- is_s3_uri(uri) ? open_s3_reader : open_file_reader
```

Mechanically, against `8876871`:

| | result |
|---|---|
| XPBatch source files changed | **0** |
| `xpq_read_row_group` | untouched |
| `xpq_row_group_stats_i64` | untouched |
| `xpq_decoded_values` | untouched |
| `class ArrowObjectInput` | untouched |
| added lines in the Arrow-facing shim | 17 — the branch plus four counter accessors |
| Arrow symbols named by the transport object | **0** |
| PostgreSQL symbols named by the transport object | **0** |

Same file, same slice, same projection, two transports:

```
local  100000 rows  6 reads  2 meta + 4 data  logical 6/749438  749438 B
s3     100000 rows  6 reads  2 meta + 4 data  logical 6/749438  749438 B
                    HEAD 1  GET 6  http_errors 0  transferred 749438 B
```

Identical, and the benchmark checksum agrees with the local file **and with the
PostgreSQL heap**: `09e54f9a0108ff447c05f2cf63ca63da|200`.

### No SDK

A blocking HTTP/1.1 client on a socket plus SigV4 over OpenSSL. One request per
read, every read on the caller's thread. That is a decision, not a gap: an SDK
thread pool is exactly where `CHECK_FOR_INTERRUPTS()` ended up on the wrong
thread in ObjectReader v0 (`docs/OBJECT_READER_V0.md` §7). With no SDK there is
no second pool to reason about — though Arrow still has one, which phase 3
covers.

No async, no cache, no prefetch, no connection reuse.

### EOF, which is the reason v1 existed

Taken from what the server states, never from a byte count:

| response | meaning |
|---|---|
| `206` + `Content-Range: a-b/total` | `eof` only when `b + 1 >= total` |
| `416` | `eof`, authoritatively — the object ends before the request |
| body shorter than its own `Content-Length` | **truncated transfer**: error, `eof` stays false |
| `200` to a ranged request | error, not a silent whole-object read |

### Refusals rather than guesses

An `https://` endpoint is refused instead of downgraded to cleartext. A response
with no `Content-Length` is refused rather than guessed at, since chunked
decoding is not implemented. A `200` to a ranged request is refused rather than
sliced, because slicing hides a misconfigured endpoint.

Credentials come from `XPB_S3_*` in the server's environment, deliberately
**not** the `AWS_*` names: these are read from the postmaster's environment, and
silently picking up a developer's real AWS credentials to send over plain HTTP
to whatever endpoint is configured is not something this should do by accident.
Configuration still has no proper home.

### SigV4, checked against botocore

The endpoint accepts unsigned requests, so it cannot verify a signature. Without
an independent check the signing code would ship unchecked. Two comparisons,
because a correct HMAC chain over a wrong canonical request still fails against
AWS:

* **canonical request**, byte for byte — identical in all four cases, and needs
  no frozen clock;
* **the whole `Authorization` header** — identical in all four cases.

Freezing botocore's clock turned out to need `get_current_datetime` patched;
both the credential scope and the `X-Amz-Date` header derive from it, and
overriding `_get_date` or presetting `request.context['timestamp']` does not
reach it. Two wrong oracles were written before that was found, and **both
looked like a bug in the code under test**. The bisect that settled it:
`sha256(my canonical request)` equals botocore's own string-to-sign hash, and my
signature over botocore's canonical request equals an independent pure-Python
implementation's.

I also had a published AWS test-vector signature in mind for the docs example.
It does not match, and since my implementation agrees with botocore, my
recollection of that constant was simply wrong. It is not cited.

---

## Phase 2 — deterministic failure semantics

Faults injected by a **proxy in front of the endpoint**, so the damage is to the
HTTP exchange rather than to a decorator inside our own reader. v1's
`FaultyObjectReader` proves the contract refuses a short read; this proves a
genuinely truncated HTTP response is what the contract sees.

Deterministic by construction: every trigger is an exchange ordinal, never a
clock or a random number.

### Retry

Bounded and minimal, inside the implementation under `read_exact`, so the Arrow
adapter never learns a read was attempted twice. A fixed small number of
attempts (default 3, clamped to 1..5), a fixed 10 ms delay, no exponential
backoff, no jitter, no circuit breaker — those are policy, and policy with no
measured need is a guess.

What is retryable is a judgement about whether another attempt could plausibly
differ: connect/send/recv failure, `5xx`, `429`, and a truncated body — yes.
`416`, other `4xx`, and `200`-to-a-range — no, another attempt gives the same
answer or the endpoint is misconfigured.

### Measured, at SQL level

| mode | rows | logical | attempts | retries | transferred |
|---|---|---|---|---|---|
| clean | 100 000 | 6 / 749 438 | 7 | 0 | 749 438 |
| 503 on 1st GET | 100 000 | **6 / 749 438** | **8** | **1** | **749 546** |
| cut 1st GET | 100 000 | **6 / 749 438** | **8** | **1** | **782 206** |
| 500 after 2 GETs | **query ERROR** | — | — | — | — |

The logical request never moves. The physical traffic does, by exactly the
discarded body — 108 bytes of error XML for the 503, 32 768 bytes of half-sent
range for the truncation. A mid-scan permanent failure fails the query rather
than returning the rows it already had.

### A defect this phase found in my own reader

With every response truncated, the **footer probe** was being classified as a
clean end of object. `read_at_most` decided `eof` from `Content-Range` *before*
checking the body against its own `Content-Length` — so any range whose
`Content-Range` reaches the end of the object, **which is every Parquet footer
read**, was reported as EOF when its transfer had been cut in half.

`read_exact` refused it, so no wrong answer escaped through that path. But
`read_at_most` is what Arrow's sequential footer read uses, and it would have
been handed `eof = true` with half a footer. "The file ends here" is precisely
the lie the v1 contract exists to prevent, and it survived in the remote
implementation for exactly the ranges that matter most.

Fixed by testing truncation first and never letting `Content-Range` override it.
Both the regression and its control are tested: a cut transfer on an
end-of-object range is a failure, and an honest short range at the object's end
is still EOF. Without the control the "fix" could have been blanket distrust of
short reads, which would make the reader unable to read a footer at all.

### Resource hygiene

Sockets held by the backend after six iterations, counted under `/proc`:

```
mode ok                 sockets=1
mode fail_get_after:2   sockets=1
mode truncate_get:1     sockets=1
```

One is the client connection. No HTTP socket leaked through any failure path.

---

## Phase 3 — threads, cancellation, lifecycle

**The one that must not repeat the v0 bug.** There is no SDK here, so there is
no SDK pool — but **Arrow still reads on its own pool**, so the transport is
reached from a worker thread anyway. Measured, with pid/tid logged at each HTTP
request (backend 4078):

```
tid 4078  1 HEAD + 2 GET    the backend thread: HEAD and the footer probe
tid 4086       42 GET       an Arrow worker: every data read
```

| invariant | result |
|---|---|
| no `CHECK_FOR_INTERRUPTS()` off the backend thread | holds: `irq_skipped_offthread` = 42 = `data_calls`, exactly |
| no `ereport`/`siglongjmp` from a worker | impossible: the transport names **0** PostgreSQL symbols |
| backend terminates cleanly | `pg_cancel_backend` and `pg_terminate_backend` mid-remote-scan both end it in ~254 ms |
| postmaster shutdown not blocked | fast shutdown during a confirmed-active S3 scan: **under 1 s** |
| per-query resources released | one socket, the client's |

### A hole this phase found, and it was mine

An endpoint that accepts a connection, promises a body and then sends nothing:

```
before:  statement_timeout = 3 s  ->  still blocked at 90 s, backend alive afterwards
after:   fails in 4 s (2 attempts x 2 s), backend gone, no leftovers
         "timed out after 2000 ms with 0 of 1048576 body bytes from 127.0.0.1"
```

My sockets had **no timeout**. The blocking `recv()` is on an Arrow worker,
where the interrupt hook deliberately does not fire — so there was no interrupt
check reachable at all. That is the v0 defect class by a different route: not an
interrupt check on the wrong thread, but none available, and the consequence is
the same unkillable backend blocking cluster shutdown.

The fix is a **deadline, not an interrupt**: `SO_RCVTIMEO`/`SO_SNDTIMEO` plus a
non-blocking connect with `poll`, because the OS connect default can be minutes.
At this depth the transport is the only thing that can carry a bound — it is the
only code holding the syscall. Both timeouts are clamped: too small and a
healthy slow object fails, too large and an unreachable endpoint is
indistinguishable from a hang.

**The limitation that remains, stated:** a cancel arriving while an HTTP request
is in flight is not seen until that request returns or its deadline expires.
Over this endpoint that is ~95 ms; over real S3 with a large range it would be
longer, bounded by `XPB_S3_IO_TIMEOUT_MS`. Cancellation granularity on the data
path is therefore the transport deadline, not the interrupt check.

---

## Phase 4 — accounting

Three levels, counted in three places, and the third only means something now
that there is a transport:

| level | where | what it counts |
|---|---|---|
| logical | `ArrowObjectInput` | what **Arrow** asked for |
| physical requests | `S3Reader` | HTTP exchanges attempted, retries included |
| bytes transferred | `S3Reader` | bytes off the wire, retries included |

Plus `s3_head_calls`, `s3_retries`, `s3_http_errors`. A local source reports
`-1` for all of them, so "the S3 path was taken" is never confused with "it fell
back to a file".

The simple retry case, which is what the brief asked to see:

```
logical:            unchanged   6 calls / 749 438 B
physical requests:  +1          7 -> 8
transferred bytes:  +108        749 438 -> 749 546
```

`size()` costs exactly one HEAD and is cached — asserted, because the contract
says the size is fixed for the reader's life.

---

## Phase 5 — Parquet correctness over S3

Every shape through `S3Reader`, through `FileReader`, and against the heap the
file was exported from where it can answer. **Identical in all of them.**

| shape | result (both transports) |
|---|---|
| full scan, 4 cols | 10 000 000 rows, sum 500 103 444 582 |
| projection, 1 col | 10 000 000 rows, 10 000 000 values decoded |
| filter and pruning | 1 050 000 rows, 21 read, 179 skipped |
| aggregate over a slice | sum 52 502 837 564, min 1, max 100 000 |
| NULLs | 0 / 0 |
| empty result | 0 rows, 0 row groups, 0 data bytes |
| single row group | 100 000 rows, 2 read |
| many row groups | 10 000 000 rows, 200 read |

```
group by   s3   09e54f9a0108ff447c05f2cf63ca63da|200
           file 09e54f9a0108ff447c05f2cf63ca63da|200
           heap 09e54f9a0108ff447c05f2cf63ca63da|200
```

The existing third-party gate (DuckDB over the same Parquet file) is unchanged
by this series and still passes against the local file; it has no S3 arm and was
not given one, since that would test DuckDB's S3 client, not ours.

---

## Phase 6 — projection and pruning save remote work

Two separate experiments, as asked. Projection at a fixed slice:

| | GET | data bytes | transferred | row groups |
|---|---|---|---|---|
| 4 columns, `[25..36]` | 44 | 5 517 323 | 5 755 016 | 21 |
| 1 column, `[25..36]` | **23** | **4 538** | **242 231** | 21 |

Same row groups, so this is projection and not pruning. Pruning at a fixed
projection:

| | GET | data bytes | row groups |
|---|---|---|---|
| all | 402 | 52 557 608 | 200 |
| one row group | 6 | 764 104 | 2 |
| **pruned to nothing** | **2** | **0** | **0** |

A row group pruned away costs **no remote data byte**. The two remaining
requests are the footer: opening a remote object is real work and is reported,
not hidden. Arrow's coalescing behaviour is unchanged and untouched — this phase
measured it, it did not tune it.

---

## Phase 7 — the shape of the cost

Not a benchmark campaign, and no claim that S3 is fast or slow. Five timed
repetitions, median.

| query | S3 wall | local wall | GET | data bytes |
|---|---|---|---|---|
| footer only (pruned to 0) | 228 ms | 18 ms | 2 | 0 |
| pruned, 1 col | 2 302 ms | 20 ms | 23 | 4 538 |
| pruned, 4 cols | 4 407 ms | 44 ms | 44 | 5 517 323 |
| full scan, 4 cols | 39 147 ms | 182 ms | 402 | 52 557 608 |
| join2 + groupby `[25..36]` | 4 335 ms | 42 ms | 44 | 5 517 323 |

**The decomposition, and a correction.** `decode_ms` reported 38 890 ms of the
39 147 ms full scan. That is not decode: it is measured around Arrow's
`ReadRowGroup()`, which is where Arrow performs its reads, so over a remote
source it is transfer *plus* decode. The same scan locally reports 148 ms, and
the data and decoding code are identical, so the local figure is the proxy for
decode. The instrument is now named for what it measures in
`xpb_parquet_shim.h`; reporting 38.9 s as "decode" would have been exactly the
kind of mislabelled number this project keeps finding.

**Where the time goes**, measured independently with a Python client rather than
inferred from our own numbers:

```
per-request latency, new connection each time, direct to moto:
     218 B   median  90.9 ms
  262 144 B  median  95.1 ms
through the fault proxy: 91.1 ms and 96.5 ms -- the proxy adds ~1-5 ms
```

Latency is **essentially independent of size**. So:

```
wall ~= 95 ms x GET count + local work
  footer only    2 x 95 =   190 ms   measured   228 ms
  pruned 1 col  23 x 95 = 2 185 ms   measured 2 302 ms
  pruned 4 col  44 x 95 = 4 180 ms   measured 4 407 ms
  full scan    402 x 95 =38 190 ms   measured39 147 ms
```

The cost is linear in **request count** and nearly independent of bytes, and our
reader adds nothing measurable on top of the endpoint's own latency. The
transferable conclusion is the shape, not the constant: for a remote Parquet
scan the lever is the number of requests, which is what projection and pruning
move (402 → 44 → 23 → 2). The obvious next optimisation is connection reuse,
and it is deliberately not in this series.

The absolute numbers are the mock's. Real S3 first-byte latency is lower and
bandwidth-bound transfer would matter more, so none of these milliseconds
transfer to AWS.

---

## Phase 8 — verdict

**A — S3Reader fits `ObjectReader` unchanged.**

Grounds, all mechanical or measured:

1. Zero XPBatch source files changed; `xpq_read_row_group`,
   `xpq_row_group_stats_i64`, `xpq_decoded_values` and `ArrowObjectInput`
   untouched. The cost above the reader is one scheme branch.
2. Identical read pattern, identical answers and identical checksums over both
   transports, agreeing with the PostgreSQL heap.
3. No contract change was needed. The v1 two-operation split — `read_exact`
   exact-or-error, `read_at_most` with authoritative `eof` — was written for a
   transport before one existed, and it held: the one defect found in this
   series was my implementation violating the contract's ordering, not the
   contract being wrong.
4. Retry has a home inside the implementation and is invisible above it, with
   its cost visible in the physical counters.
5. The three accounting levels fit the existing counter scheme.

Not B: nothing needed to change in the transport-general interface. Not C: no
remote notion leaked above `ObjectReader` — the adapter has no retry policy, no
timeout, no endpoint and no credentials, and the operators do not know a byte
source exists.

### Remaining gaps

These block real S3 use and none of them is hidden behind "later":

* **No TLS.** `http://` only, and `https://` is refused rather than downgraded.
  This cannot talk to AWS. It is the single largest gap.
* **SigV4 is unverified against a real signer's server.** It matches botocore
  byte for byte on canonical request and `Authorization` header; it has never
  been accepted by AWS.
* **No connection reuse.** Phase 7 shows the cost is per-request, so this is
  the first thing worth doing and the first thing that will change the numbers.
* **Cancellation granularity on the data path is the transport deadline**, not
  the interrupt check, because the read is on an Arrow worker. Bounded and
  stated; not improvable without taking the reads off Arrow's pool.
* **Configuration has no home.** Environment variables read from the
  postmaster, which is not where credentials belong.
* **Not implemented:** chunked transfer-encoding (refused), redirects,
  virtual-host-style addressing (path style only), IPv6 literal hosts,
  multipart or parallel range fetching, `If-Match`/ETag consistency checks.
* **No object-store consistency story.** Nothing detects an object replaced
  between the footer read and a data read; over S3 that would silently mix two
  versions. `size()` being cached makes the window wider, not narrower. This is
  the one gap I would raise to the top of a phase 9, because it is a
  wrong-answer class rather than a missing feature.
* **The mock is not S3.** moto accepts unsigned requests, its latency is ~95 ms
  regardless of size, and it has no eventual-consistency or throttling
  behaviour.
