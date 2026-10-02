# Parquet over object storage — design note (0006)

Nothing here is implemented. This note exists to record what the local Parquet
source measured about I/O shape, and what would and would not have to change for
the same provider to read from an object store.

Scope: design note only. No `ObjectReader` type, no S3 code, no GUC, no
configuration surface is added by this commit.

## What the local implementation measured

From the 0005 instrumentation (a forwarding wrapper over
`arrow::io::RandomAccessFile`, split at the first column-chunk read so footer
parsing does not mix with data volume), on
`benchmarks/08-parquet-source/data/orders.parquet` — 8 row groups × 25 000 rows,
10 columns, uncompressed, PLAIN:

| case | row groups read | data bytes | read calls |
|---|---|---|---|
| 1 column, no predicate | 8 | 800 960 | 9 |
| 2 columns, no predicate | 8 | 2 996 901 | 17 |
| 2 columns, predicate selecting 1 row group | 1 | 299 493 | 3 |
| 2 columns, predicate excluding every row group | 0 | 0 | 1 |

`meta_bytes` is 65 536 in one call in all four cases, including the last.

The shape is exact, not approximate:

    read_calls = 1 (footer) + row_groups_read * projected_columns

One request per selected column chunk, one request for the footer. No
coalescing, no read-ahead, no over-read: `data_bytes` equalled
`attributed_bytes` — the compressed size the footer attributes to the selected
chunks — to the byte.

## Why that shape is the whole object-storage question

Projection and pruning move **bytes**. The local measurements show they do that
well: 3 of 10 columns and 1 of 8 row groups cut data bytes from 12 MB of file to
299 KB of transfer.

Object storage prices a second axis the local file does not: **requests and
round trips**. 17 reads of a local file are 17 `pread()` calls against the page
cache and cost nothing worth measuring. 17 sequential `GetObject` range requests
at a per-request latency of 20–80 ms cost 0.34–1.36 s before a single byte of
decode — more than the transfer of 3 MB on any plausible link. The byte-optimal
plan and the request-optimal plan are not the same plan.

Two consequences follow directly from the table above and are worth stating
before any code:

1. **The footer read is unavoidable and is on the critical path.** The
   prune-to-nothing case transfers zero data bytes and still costs 65 536 bytes
   in one request. Pruning cannot remove the open; remotely, a query that reads
   nothing still pays one round trip. Footer caching is therefore the first
   thing anyone would want, and it is a correctness question (invalidation), not
   only a performance one.

2. **Request count grows with the product of row groups and projected
   columns.** Locally that product is free. Remotely it is the dominant term for
   a wide projection over many row groups, which is precisely the shape a
   columnar scan produces. Coalescing adjacent chunks and issuing requests
   concurrently are the two obvious answers; both are future questions, and
   neither is implemented.

## The reader surface

The local implementation already isolates all I/O behind one Arrow interface.
`CountingFile` in `xpb_parquet_shim.cpp` is a forwarding wrapper over
`arrow::io::RandomAccessFile`, and Arrow's Parquet reader goes through that
interface for every byte it reads. An object-store reader is a second
implementation of the same interface, injected where the local one is
constructed today.

So `ObjectReader` is not a new abstraction to invent. It coincides with the
shape Arrow already requires:

    GetSize()                     total object size
    ReadAt(offset, nbytes)        one range read
    Close() / cancellation
    metrics counters              bytes, calls, phase

Deliberately **not** in that surface: a predicate interface, a schema
interface, a cache policy, a scheduler, an async future type, a credential
object. Each is a design decision that belongs to whoever has a measurement
demanding it. A reader that can report its size, read a range, be closed, and
count what it did is enough to carry Parquet, and widening it now would be
designing for a caller that does not exist.

The instrumentation added in 0005 is counted at this wrapper, not inside the
provider, so `data_bytes` and `read_calls` keep their meaning unchanged under a
remote reader. That is one thing that does not have to be redone.

## Prefetch

The current implementation is single-buffered: the shim holds one row group's
`arrow::Table` and releases it before reading the next, which is what makes the
borrow window identical to the batch contract's window.

That is a property of this implementation and not a restriction imposed by the
contract. To state it exactly, because it was previously recorded the wrong way
round:

> Current implementation is single-buffered; the batch contract permits prefetch
> provided prefetched data has independent lifetime.

"Independent lifetime" is the whole condition. The contract's borrow window runs
until the next `next_batch()`/`rescan()`/`end()`. A prefetched row group must
therefore live in its own allocation, not in a slot that the current batch's
columns point into — otherwise starting the next read invalidates columns the
operators are still reading, which is a use-after-free and not a slow path.
Single-buffering satisfies that by holding only one row group at a time;
double-buffering would satisfy it by holding two independent ones. Nothing in
`xpb_colbatch.h` has to change either way.

Prefetch is named here as a future question. It is not implemented, and no
counter in the provider anticipates it.

## Local decisions that would block object storage

Listed because they are cheaper to name now than to discover later. These are
not bugs in the local path; they are places where the local path assumes a local
file.

1. **The shim opens the file itself.** `xpq_open()` constructs the local reader
   inside the C++ half. A remote reader has to be injected rather than
   constructed there, which means `xpq_open()` gains a reader argument or a
   factory. This is the one structural change; everything below it is policy.

2. **Open and every read are synchronous and uninterruptible.** Arrow's call
   stack contains no `CHECK_FOR_INTERRUPTS()`, so a hung remote request would
   leave a backend that neither `statement_timeout` nor `pg_terminate_backend`
   can end — the same failure shape as the R1-12 planner hang
   (`docs/roadmap/groupagg2-stream-exit-unsound.md`), and that one held a
   relation lock for 39 minutes. **This is a production blocker for remote I/O,
   not a performance note.** It is acceptable now only because a local
   `pread()` from the page cache cannot hang. Any S3 work must carry a timeout
   and a cancellation path before it carries a benchmark.

3. **No retry or timeout policy, and no classification of errors.** The ABI
   reports failure as a string in `errbuf`. Locally that is adequate: a local
   read either works or the file is wrong. Remotely, retryable (throttling,
   connection reset) and terminal (missing object, denied) failures need to be
   distinguishable, or the provider either gives up on transient errors or
   retries permanent ones.

4. **No credential or endpoint surface exists.** The request carries a `uri`
   string and nothing else — no region, no endpoint, no profile, no
   credentials. `XpbSourceRequest.uri` is already a string, so the registry side
   needs no change; the configuration surface is entirely absent and would be
   new.

5. **The footer is re-read on every open.** With one local `pread()` that is
   invisible. Remotely it is a round trip per query, and caching it introduces
   an invalidation question (an object store has no relation-level lock to hang
   invalidation on).

6. **`uri` is interpreted as a filesystem path by the provider.** Nothing
   outside the provider knows that, which is the point of the registry — but the
   provider does assume it, so scheme dispatch (`file://`, `s3://`) would live
   there.

## Conclusion

> Local Parquet did not require a Parquet-aware executor. It only required a
> provider that supplies projected typed columns. Object storage changes how
> those bytes arrive, not the XPBatch contract.

What object storage does change is the cost model of arrival — requests and
latency rather than bytes — and the interruptibility of a backend that is
waiting. The first is an optimisation question. The second is a safety question
and has to be answered first.
