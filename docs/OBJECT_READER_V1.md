# ObjectReader v1 — what a read means when the source is unreliable

No network, no S3, no coalescing work. Base commit `cdbbf6a`, branch
`objectreader-v1`.

The phase exists because v0 left one question open and it was the dangerous
one. Answering it before a remote reader exists is the point: otherwise
transport, semantics and accounting all get invented at once, and the first two
leak back into the Parquet shim.

---

## 1. The defect in the v0 contract

v0 had one read operation, documented as "exact, except at end of object":

```
read_at(offset, n) -> int64
      n bytes   -> success
    < n bytes   -> the object ended here
         -1     -> failure
```

The caller infers EOF from a short return. That inference is **correct for a
local file** — `pread()` returning 0 means the file ended, and the kernel is not
a transport that can lose bytes. It is **wrong for anything over a transport**: a
ranged GET can return fewer bytes than asked while the object continues.

So under the v0 contract a dropped connection would have arrived as a clean end
of object. For a column store that is the worst failure shape there is: not a
crash, but a **silently short column chunk** feeding a correct-looking
aggregate.

## 2. The empirical question the fix depended on

Whether exact-or-error is usable at all depends on whether Arrow ever
legitimately asks for a range that runs off the end. Measured before designing,
with offset/length/size logged at the `pread`:

```
off=180077809 n=65536  size=180143345   footer probe -- inside
off=179971180 n=172157 size=180143345   footer        -- inside
off=35997325  n=218    size=180143345   column chunk  -- inside
off=36093826  n=255368 size=180143345   column chunk  -- inside
```

No request ever ran past the end, and across every shape measured in v0
`bytes_requested == bytes_returned` — no read was ever short. Arrow computes its
ranges from the footer, so they are known to be inside the object.

**Exact-or-error is therefore viable**, and the `ReadResult{bytes, eof}`
alternative is not needed for the positional path.

## 3. The contract

Two operations, because one signed count could not carry two independent facts.

```cpp
struct ReadResult { int64_t bytes; bool eof; bool ok; };

bool       read_exact(int64_t offset, int64_t nbytes, void *out);
ReadResult read_at_most(int64_t offset, int64_t nbytes, void *out);
int64_t    size();
void       close();
```

**`read_exact`** — exactly `nbytes` or failure. Never short, never partially
satisfied, and **no EOF concept at all**: a request running off the end is an
error, because the caller asked for bytes that do not exist. This is what the
Arrow adapter uses for every positional read.

**`read_at_most`** — up to `nbytes`, for the one caller that does not know how
much is there. `eof` is **authoritative**: an implementation sets it only when it
knows the object ended, and **must never set it merely because it received less
than it asked for**. A short result with `eof == false` is a transport failure,
and `read_exact` built on top of it reports an error rather than success.

EOF is never inferred. It is either stated by the implementation or it is not
EOF.

`read_exact` is a **non-virtual-by-default base implementation** — it asks
`read_at_most` and refuses anything short — so the refusal logic exists once and
every implementation inherits it. An implementation overrides it only if its
transport has a cheaper exact-read primitive. The two short cases are reported
differently because they are different faults:

```
read past end of object: wanted 218 at 35997325, object ends after 0
short read with no end of object: wanted 218 at 35997325, got 109
    -- truncated transfer, not EOF
```

**Retry is the implementation's business, underneath `read_exact`.** The adapter
has no retry policy and must not grow one. That is what keeps transport concerns
out of the Parquet shim.

### Where the adapter uses which

| Arrow method | operation | why |
|---|---|---|
| `ReadAt(pos, n, out)` | `read_exact` | a range computed from the footer; short means fault |
| `ReadAt(pos, n)` → Buffer | `read_exact` | same |
| `Read(n, out)` | `read_at_most` | Arrow's stream shape, where short is a legitimate answer |
| `Read(n)` → Buffer | `read_at_most` | same |

## 4. FaultyObjectReader

A reader that fails on purpose, wrapping a real one — same file, same bytes,
same decode path, **damaged delivery only** — so every answer stays checkable
against the PostgreSQL oracle.

Deterministic by construction: the trigger is a read **ordinal**, not a clock or
a random number, so a failing case fails on every run.

| mode | what it does |
|---|---|
| 1 `kShortNoEof` | fewer bytes, `eof` **not** set — the case v0 would have accepted as a clean end |
| 2 `kTransientFirst` | the first attempt fails, a retry succeeds |
| 3 `kFailAfterNBytes` | delivers *n* bytes into the buffer, then fails |
| 4 `kGenuineEof` | reports the real end of the object, truthfully |
| 5 `kChunked` | satisfies one read in several short pieces, none claiming `eof` |

Mode 2's failing attempt does **real work and is discarded**, because that is
what a transient transport failure costs. The first version skipped it, which
made the retry free and therefore invisible in the attempt count — the test
would have shown nothing. Caught by the test failing with "the retry is
invisible in the physical counters … it was free, so nothing was tested".

Reached from SQL through the real provider path by a one-shot armed fault
(`xpq_arm_fault`), consumed by the next open and then disarmed. Deliberately not
a GUC: a GUC would look like a supported knob, and this is a test hook.

## 5. Two levels of accounting, separately counted

The brief asked for three quantities. Two exist and the third needs a transport
before it means anything:

* **logical** — what Arrow asked for, counted in the adapter, which is the layer
  Arrow's request arrives at;
* **physical** — what the reader actually did, counted in the reader;
* bytes actually transferred — identical to physical for a local reader, so no
  counter pretends otherwise.

One range read, same file, same slice:

| fault | logical | physical | rows |
|---|---|---|---|
| none | 6 calls / 749 438 B | 6 calls / 749 438 B | 100 000 |
| 2 transient + retry | **6 calls / 749 438 B** | **7 calls / 749 656 B** | 100 000 |
| 5 chunked | **6 calls / 749 438 B** | **10 calls / 749 438 B** | 100 000 |

A retry costs one extra call and 218 extra bytes of physical traffic and changes
neither the logical request nor the answer. Chunking moves the same bytes in
more calls. That is the acceptance criterion demonstrated rather than asserted,
and it is only visible because the two levels are counted in different places.

## 6. Acceptance criteria

All five, from `extension/xp_batch/test/object_reader.sh` §8 — **45 correct, 0
wrong** for the whole file:

| criterion | result |
|---|---|
| transport short read ≠ EOF | refused, in those words |
| real EOF == EOF | reported as EOF, **distinguishably** from a short read |
| retry does not change logical `requested_bytes` | 6 calls / 749 438 B, unchanged |
| physical may change because of retries | 6 → 7 calls, 749 438 → 749 656 B |
| corruption never becomes a valid shortened column | modes 1/3/4 fail the query; modes 2/5 return the **bit-identical** checksum |

The "real EOF == EOF" case is the control, and it is not decoration: if the fix
had been "distrust every short read", that case would fail too and the reader
would be unable to tell the two apart — which is the entire reason `eof` is
carried explicitly.

At query level, against the oracle checksum rather than a row count:

```
clean                                09e54f9a0108ff447c05f2cf63ca63da|200
mode 2 (recoverable)   identical     09e54f9a0108ff447c05f2cf63ca63da|200
mode 5 (recoverable)   identical     09e54f9a0108ff447c05f2cf63ca63da|200
mode 1 / 3 / 4         query fails rather than returning short data
```

Unchanged by this phase: benchmark 08 **14/0** against PostgreSQL and pyarrow
and **41/0** for the provider; benchmark 02 `gate PASS` and `third-party gate
PASS`; `pruning-control` same checksum from clustered and shuffled files.

## 7. The thread-safety defect found on the way in

Not part of the planned scope, and the reason this phase started with a stop
rather than with code. Committed separately as `2eb0666`; recorded in full in
`docs/OBJECT_READER_V0.md` §7, which this phase also corrected because its
cancellation section stated a safety property the code did not have.

In short: the v0 interrupt hook called `CHECK_FOR_INTERRUPTS()` before every
physical read, and Arrow's `pre_buffer` performs column-chunk reads on its
thread pool — so the macro ran on a non-backend thread for 42 of 44 reads on a
pruned scan. PostgreSQL's interrupt machinery is not thread-safe; one captured
backtrace shows an Arrow worker running `proc_exit` →
`ThreadPool::Shutdown()` on its own pool while the backend thread waited in
`ReadRangeCache::Read()` for that worker's read. Unkillable backend, blocked
cluster shutdown.

Fixed by running the hook only on the installing thread, and making the counters
atomic. Cancel latency is unchanged at 32–34 ms against 291–373 ms, which shows
the hook on the data path was never the mechanism by which cancellation worked.

## 8. What this closes, and what it does not

**Closed:**

* EOF can no longer be inferred from a short read, anywhere.
* A real post-open `ERROR` path in the source is now deterministically
  triggerable and tested, including that it leaks no descriptor — 3 injected
  errors, 0 descriptors. Before this, the only post-open error available was a
  column name absent from the file, which fails before any data read.
* Retry has a defined home (the implementation) and a measurable cost.

**Not closed, and not claimed:**

* **The shim's multi-chunk fallback is still unexercised.** Mode 5 chunks the
  *byte delivery*; the shim's concatenate branch fires when **Arrow** returns a
  column as several arrays. Different thing, and the counters say so:
  `copy_bytes = 0` and `arrow_chunks = 4` under every mode, exactly as without
  a fault. I had expected mode 5 to reach it and it does not.
* **The post-open `ERROR` window inside `xpq_columns`** remains reasoned-only.
  `xpq_columns` performs no read after open, so no read fault can reach the
  window between its open and its close; it needs fault injection in
  `CStringGetTextDatum`/`tuplestore_putvalues`, which does not exist.
* **No S3, no transport.** `read_at_most`'s contract now says what a remote
  implementation must not do, but nothing enforces it on an implementation that
  does not exist yet.
* **Timeouts** are not in the contract. A transport needs them and they are not
  a local-file concern, so they are deliberately absent rather than guessed at.

## 9. Verdict

**A — the contract is right and the fault reader is the way to keep it right.**

Grounds:

1. The ambiguity v0 shipped is gone at the type level, not by convention: there
   is no operation that can return a short count a caller might read as EOF.
2. The two short cases are distinguishable in the error text and in tests, and
   the real-EOF control proves the fix is not blanket distrust.
3. Retry has one home, and its cost is visible in the physical counters while
   the logical request is provably untouched.
4. Unrecoverable faults fail the query; recoverable ones return a bit-identical
   checksum. That is the silent-short-column class closed by test, not by
   argument.
5. The decision was made from a measurement — Arrow never asks past EOF — rather
   than from a preference between two candidate signatures.

Not A because it is complete: §8 lists what is open, and one item there is an
expectation of mine that turned out to be wrong.
