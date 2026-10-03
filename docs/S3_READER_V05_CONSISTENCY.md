# S3Reader v0.5 — a real S3 server, and one object version per reader

Base commit `219358f`. Branch `s3-reader-v05`.

Two things this phase is for: replace the mock with a real open-source
S3-compatible server that actually verifies signatures, and close the gap v0
named as its most serious — a footer read from one incarnation of an object and
data ranges from another.

---

## 1. The server

| | |
|---|---|
| server | **RustFS**, `github.com/rustfs/rustfs` |
| version | tag `v1.0.0-beta.1`, commit `8e1bd560` |
| build | `cargo build --bin rustfs --no-default-features`, rustc 1.95.0, 40 m 35 s |
| run | `rustfs server /tmp/rustfs-data --address 127.0.0.1:9000 --access-key … --secret-key … --region us-east-1` |
| addressing | path style |
| TLS | none; plain HTTP on loopback |

### Why not VersityGW

VersityGW was the first preference and cannot be built in this environment.
Both reasons are egress policy, not a problem with the project:

* its `go.mod` requires Go 1.26.0 against an installed 1.24.7, and the
  toolchain download comes from `proxy.golang.org`, which this session's egress
  policy refuses (403);
* its dependencies include `golang.org/x/…`, `go.yaml.in/…` and
  `gopkg.in/yaml.v3`. With `GOPROXY=direct` those resolve to `golang.org` and
  `gopkg.in`, both also refused (403).

The proxy README is explicit that a 403 is a policy denial to report rather
than route around, so I did not look for a way past it.

### Why the RustFS beta tag and not the tip

The tip (`f071429`) requires rustc 1.98.1; the installed toolchain is 1.95.0
and new toolchains come from `static.rust-lang.org`, also refused.
`v1.0.0-beta.1` declares `rust-version = "1.93.0"` and builds. `protoc` was
missing for the `pulsar` crate's build script and came from the Ubuntu archive.

### Auth is really verified — the point of the exercise

moto accepted unsigned requests, so v0 could only check SigV4 against botocore.
RustFS does not:

```
unsigned GET                     -> 403 AccessDenied
unsigned HEAD                    -> 403
correct key, wrong secret        -> 403 SignatureDoesNotMatch
unknown access key               -> 403 InvalidAccessKeyId
correct key, one signature byte flipped -> 403 SignatureDoesNotMatch
correct credentials and signature       -> 206, 16 bytes, "PAR1"
```

**So the reader's own SigV4 is now verified by a server**, not only compared
with another implementation. That closes a v0 gap outright. It also means the
signature over the `If-Match` header added in this phase is genuinely checked:
a wrong canonicalisation would be rejected, not silently tolerated.

---

## 2. The existing reader against the real server

Nothing architectural changed for this. Same file, same slice, same
projections:

| query | GET | HEAD | data bytes | row groups |
|---|---|---|---|---|
| footer only (pruned to 0) | 2 | 1 | 0 | 0 |
| single row group | 6 | 1 | 749 438 | 2 |
| projection, 1 col `[25..36]` | 23 | 1 | 4 538 | 21 |
| pruned, 4 cols `[25..36]` | 44 | 1 | 5 517 323 | 21 read, 179 skipped |
| full scan, 4 cols | 402 | 1 | 52 557 608 | 200 |

Identical to the figures against moto and to the local `FileReader`. The
accounting counters behave as before: `logical 6/749438`, `transferred
749438`, `retries 0` on a clean read.

```
group by   rustfs 09e54f9a0108ff447c05f2cf63ca63da|200
           file   09e54f9a0108ff447c05f2cf63ca63da|200
           heap   09e54f9a0108ff447c05f2cf63ca63da|200
```

A full scan takes 8.8 s for 402 requests — about 22 ms each, roughly four times
faster than the mock. The cost is still linear in request count, as v0's
characterisation said.

No stop condition: the real server required no change to `ObjectReader` or to
`ParquetReader`.

---

## 3. What the server gives for identity

HEAD returns exactly these headers: `content-length`, `content-type`, `date`,
`etag`, `last-modified`, `x-request-id`. No `VersionId` (bucket versioning is
not enabled), no checksum headers, and notably **no `accept-ranges`** although
ranges work.

Conditional reads, probed independently of our reader:

| request | result |
|---|---|
| `GET` + `Range` + `If-Match: <current etag>` | 206, correct `Content-Range` |
| `GET` + `Range` + `If-Match: <other etag>` | **412 PreconditionFailed** |
| `GET` + `Range` + `If-None-Match: <current>` | 304 |
| `HEAD` + `If-Match: <other>` | 412 |

So the preferred model is available:

```
open -> HEAD -> capture (size, etag)
every Range GET -> If-Match: <etag captured at open>
mismatch -> 412 -> query ERROR
```

### ETag is an opaque identity token, not a content hash

**This is relied on explicitly, and the evidence for it came out of this
phase.** The same 180 143 345 bytes carried three different ETags depending
only on how they were written:

```
multipart upload (22 parts)   "c7333b8e11884cda65957f5f6e7e62c4-22"
server-side copy              "0d8da64d7ccb93afad60f721b6594e9d"
server-side copy of object B  "5492fdb15fa68d33c8c54f0a2765cac6"
```

The first has a `-22` part-count suffix and is not an MD5 of the content at
all; the second is the *same content* with a single-part ETag. Anything that
compared an ETag with a checksum we computed, or across objects, would be
wrong. The reader only ever echoes it back in `If-Match`, and the code says so
where it is captured.

---

## 4. The guard

On open, the HEAD response yields **size and identity together**, stored in one
assignment from one response. Every subsequent *range* read carries
`If-Match: <that identity>`. The HEAD itself carries none — it is what defines
identity, so conditioning it on itself would be circular.

A 412 is an explicit, final error:

* **no retry** — another attempt asks the same question and gets the same
  answer, so 412 is deliberately not in the retryable set;
* **no re-HEAD**, no adopting the new ETag, no reopening;
* no partial result.

`If-Match` is a *signed* header, sorted between `host` and `range` in the
canonical request. The canonical-request comparison against botocore still
matches byte for byte for unconditional requests, and the conditional ones are
checked by the server itself.

If the server returns **no ETag**, the reader **refuses to open** rather than
reading unguarded, because without an identity token a mixed-version scan
cannot be noticed. `XPB_S3_IDENTITY_GUARD=off` exists only so the negative
control below is reproducible; it is not a tuning knob. That refusal path is
not theoretical — it fired for real when the fault proxy forwarded HEAD without
`ETag`, which is how that omission in the proxy was found.

### Invariant

One `ObjectReader` instance reads exactly one incarnation of the object, or
fails.

---

## 5. The replacement test

A and B are as close as they can be while still differing: same schema, same
200 row groups, 50 000 rows each, SNAPPY, written with the same parameters.
They differ by **one byte** in total length (180 143 346 vs 180 143 345) and in
the values of one column, `sum(amount_dt)` 10 500 103 444 582 against
500 103 444 582. So a read that continued into B would technically work rather
than fail immediately on structure.

### Negative control — without the guard

Replacing the object two seconds into a full scan:

```
ERROR:  reading row group 53 of "s3://xpb/reg_buh.parquet" failed
DETAIL: IOError: Couldn't deserialize thrift: TProtocolException: Invalid data
        Deserializing page header failed.
```

Row groups 0–52 were served from A; row group 53 was read from B at an offset
computed from A's footer. **Mixed-incarnation reads are possible.** The same
scan without a replacement completes all 200 row groups, so the difference is
the replacement and nothing else.

Note the shape of that failure: it is indistinguishable from a corrupt Parquet
file. A reader of that message would go looking for data corruption.

### With the guard

```
ERROR:  reading row group 54 of "s3://xpb/reg_buh.parquet" failed
DETAIL: IOError: GET /xpb/reg_buh.parquet bytes=48595124-48602668: the object
        changed since this read began (If-Match "0d8da64d7ccb93afad60f721b6594e9d"
        -> HTTP 412). One reader reads one object version; not retrying and not
        reopening
```

Attributable instead of mysterious, and that is most of the value. No retry
against B, no reopen, no new ETag accepted, no partial result.

---

## 6. Replacement during a retry

The harder case, and the reason it matters is that retry already lives inside
`S3Reader`:

```
GET against A -> transient 503 -> object replaced by B -> retry
```

Driven deterministically by a proxy mode that fails a chosen data GET *and*
replaces the object before the retry arrives. The request trace shows the
mechanism:

```
HEAD n=1 g=0  range=None                              if-match=None
GET  n=2 g=1  range=bytes=180077809-180143344         if-match="0d8da64d..."
GET  n=3 g=2  range=bytes=179971180-180143336         if-match="0d8da64d..."
GET  n=4 g=3  range=bytes=35997325-35997542           if-match="0d8da64d..."   <- 503 + replace
```

The footer probe already carries the pinned identity; the HEAD alone does not.
The retry of that same range came back **412** and the query failed:

```
rows=... never returned
ERROR: ... the object changed since this read began (If-Match "0d8da64d..." -> HTTP 412)
object afterwards: 180143346 "5492fdb15fa68d33c8c54f0a2765cac6"   (B)
```

Acceptance: logical request unchanged, the retry happened, B was rejected, the
query errored. The retry cannot do otherwise by construction — the header is
rebuilt each attempt from the identity captured at open, and `size()` is called
once, so there is no path that re-HEADs mid-read.

---

## 7. Size and identity are one snapshot

They are read from the same HEAD response and assigned together, so there is no
state in which the reader holds a length from one incarnation and reads ranges
from another. Observable: exactly one HEAD per open, that HEAD carrying both,
and any range read after a change refused rather than served.

```
one open: HEAD=1  identity="0d8da64d7ccb93afad60f721b6594e9d"  rows=100000
```

---

## 8. The v0 properties still hold

Re-run against RustFS with the guard active — **69 correct, 0 wrong** for the
whole suite:

| property | result |
|---|---|
| short ≠ EOF | a cut transfer that cannot be recovered says it is not EOF |
| retry hidden above `ObjectReader` | logical `6/749438` unchanged; attempts 7 → 8, retries 1 |
| retry's cost visible | transferred 749 438 → 749 546 (503 body), → 782 206 (half-sent range) |
| permanent mid-scan failure | query errors, no partial result |
| deadline bounds a stalled transport | 46 s, which is 3 attempts × a 15 s I/O timeout |
| no interrupt check off the backend thread | `irq_skipped_offthread` = 42 = `data_calls`, exactly |
| cancel / terminate mid-scan | backend ends in under 500 ms |
| projection reduces remote work | 44 → 23 requests at the same 21 row groups |
| pruning reduces remote work | 402 → 6 requests; pruned-away groups cost **0** data bytes |
| checksum | agrees with the local file and the PostgreSQL heap |

Local gates unaffected: `object_reader.sh` 45/0, benchmark 08 14/0 and 41/0,
benchmark 02 `gate PASS` and `third-party gate PASS`.

---

## 9. A finding that is not about S3

`ObjectReader`'s API did not change, so the identity concept lives entirely
inside `S3Reader`. That raised the obvious question, and it is worth answering
with a measurement rather than an opinion: does `FileReader` have the same
exposure?

It does. Open a `FileReader`, read, overwrite the file in place through another
descriptor, read the same offset again:

```
size at open      : 180143345
size now (cached) : 180143345
bytes at open     : PAR1...
bytes after write : XXXX...
-> ONE READER SAW TWO INCARNATIONS.
```

Deterministic, no race. A rename would be safe — the open descriptor keeps the
old inode — but an in-place overwrite, which is what a careless refresh does,
is not. So the mixed-version wrong-answer class is **not S3-specific**, and the
guard built here closes it for one transport only.

(A racing attempt through SQL was inconclusive rather than negative: a local
full scan finishes in ~180 ms, faster than a 180 MB overwrite reaches the later
offsets. The direct-API probe above is what settles it.)

---

## 10. Verdict

**A — the real server confirms the existing transport contract, and
ETag/If-Match gives one-version-per-reader semantics.**

* `ObjectReader` API changes: **none**. `size()`, `close()`, `read_exact()` and
  `read_at_most()` are untouched; the guard is entirely inside the S3
  implementation.
* `ParquetReader` and the XPBatch operators: untouched, as in v0.
* A server that enforces SigV4 accepted the reader unchanged, including the
  added signed header.
* `If-Match` on ranged GET gives exactly the invariant asked for: one reader,
  one object version, or an error.

Not B *for S3*: no transport-general concept was needed to make S3 safe. Not C:
nothing about S3 consistency leaked above `ObjectReader` — the adapter and the
operators still know nothing of endpoints, retries, deadlines or identity.

**But §9 is the argument for B as the next step**, and it is now an evidenced
one rather than a guess: the same wrong-answer class exists on `FileReader`, so
a small transport-general identity concept on `ObjectReader` — "pin an opaque
token at open, require it on every read, fail if it moves" — would close it
once instead of per transport. I would not have proposed that before measuring
it; the local probe is what makes it worth proposing.

### Remaining gaps

* **`FileReader` has no identity** (§9). Same class, unguarded. The strongest
  candidate for the next phase.
* **No TLS**, still. Plain HTTP on loopback, `https://` refused rather than
  downgraded. This cannot talk to AWS.
* **No bucket versioning.** RustFS returned no `VersionId`, so identity rests
  on ETag alone. With versioning enabled, `versionId` would be a stronger
  token, and a reader could pin a specific version rather than merely detect
  change. Untested here.
* **The guard costs nothing measurable but was not benchmarked.** `If-Match`
  adds one signed header per request; no timing claim is made.
* **Conditional-read behaviour is RustFS's.** Real S3's `If-Match` on ranged
  GET is specified, but not tested here, and other implementations may differ
  — MinIO, Ceph and AWS are all untested.
* **A 412 ends the query.** There is deliberately no reopen-and-restart, which
  for a long analytical scan over a frequently rewritten object means repeated
  failure rather than progress. That is the right default for correctness and
  the wrong default for availability; choosing otherwise is a policy decision
  above this layer and is not taken here.
* **No connection reuse**, unchanged and deliberate. v0 showed cost is linear
  in request count, so this is the obvious performance step — after this
  correctness property, not before.
