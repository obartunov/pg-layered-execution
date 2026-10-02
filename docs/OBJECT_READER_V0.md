# ObjectReader v0 — a byte-range boundary under the Parquet reader

Local file only. No S3 code exists in this phase and none was written.

Base commit `a08893b`. Branch `objectreader-v0`.

---

## 1. Why a boundary, and where exactly it is

Before this phase the Parquet reader was handed a pathname and called
`arrow::io::ReadableFile::Open()` on it. Local-file mechanics and Parquet
decoding therefore lived in the same translation unit, and the question "which
layer decided to read these bytes?" had no answer in the code — only in a
reader's memory of it.

The layering now is:

```
    XPBatch operators          rows, batches, types
        |
    Parquet provider           projection, row-group pruning, batch assembly
        |                      (xpb_parquet_module.c)
    ParquetReader (Arrow)      WHICH byte ranges are needed
        |
    ArrowObjectInput           translation only, 60 lines
        |
    ObjectReader               fetch bytes at (offset, length)
        |
    FileReader                 open/fstat/pread/errno, and nothing else
```

Two properties this is supposed to buy, both of which are measured in §6:

* projection and row-group pruning decide which ranges are fetched, and that
  shows up as fewer physical reads, counted at the bottom;
* a second implementation of `ObjectReader` can be added without touching
  `ParquetReader` or any XPBatch operator. §8 answers that from the code
  rather than asserting it.

### Direction of the dependency

`ObjectReader` does **not** inherit from `arrow::io::RandomAccessFile`. Arrow's
interface carries `Tell`/`Seek`, a stream position and a `Status`/`Result`
vocabulary that a byte-range object store has no opinion about. Inheriting from
it would mean a future `S3Reader` is written against Arrow rather than against
this contract, which is the thing the boundary exists to prevent.

The Arrow shape is satisfied by translation instead, in one direction only:
`ArrowObjectInput` presents an `ObjectReader` **as** a `RandomAccessFile`. It is
the only Arrow-shaped code in the byte path. Arrow's sequential `Read()` is
emulated as `read_at(pos_)` plus a cursor bump, because the Parquet footer probe
uses it.

Mechanical check — the reader object file names no Arrow symbol:

```
$ nm -C extension/xpb_parquet/src/xpb_object_reader.o | grep -ci arrow
0
```

The Makefile rule for `xpb_object_reader.o` is written without the Arrow include
path, so the file cannot acquire one by accident.

C++ rather than C: the one implementation that exists is local, and the one that
is expected (S3) would be built on a C++ SDK. Nothing in `ObjectReader` is on
the C ABI, so a virtual call costs nothing a C function pointer would not.

---

## 2. The API, derived from what Parquet actually asks for

Arrow's Parquet reader needs exactly three things from a file: its size, a read
at an absolute offset, and a close. The footer probe additionally reads
sequentially, which the adapter emulates. So:

```cpp
virtual int64_t size() = 0;
virtual int64_t read_at(int64_t offset, int64_t nbytes, void *out) = 0;
virtual void    close() = 0;
```

Nothing else was added. In particular there is no `prefetch`, no `read_ranges`
and no async variant: none is reachable from the current reader, and a
capability that nothing exercises is a claim, not a feature.

---

## 3. Semantics, pinned

Spelled out in `xpb_object_reader.h` because "obvious" is how the previous
boundary acquired its assumptions.

| Question | Answer | Why that one |
|---|---|---|
| Exact or short read? | **Exact, except at end of object.** Returns `nbytes` when `nbytes` are available; fewer **only** because the object ends there. | Parquet's footer probe reads backwards from the end and a short read is its normal answer. A caller that had to loop would be writing retry logic at every call site. |
| Is EOF an error? | **No.** A read entirely past the end returns 0. | EOF is "no more bytes here", never a failure in itself. |
| `offset < 0`, `nbytes < 0`, `offset + nbytes` overflowing int64? | **Errors, not clamps.** `range_ok()` in the base class, so every implementation refuses the same things identically. | A clamped range reads the wrong bytes and reports success. That is a silent wrong answer, which is the defect class this project keeps finding. |
| `nbytes == 0`? | Returns 0 without touching the object and **without counting a read**. | A zero-length read is not I/O, and counting it would corrupt the instrument. |
| Who owns the buffer? | **The caller provides it.** The reader never allocates and never hands memory back. | No ownership question and no lifetime tied to a returned buffer. |
| Is `size()` stable? | **Fixed for the life of the reader.** | A local file that grows underneath is not supported, and a Parquet file whose footer moved is corrupt by the time anyone notices. |
| `close()` twice? | **Idempotent.** A read after close is an error, not undefined. | `end()`-not-idempotent was a real bug in this project, found by the conformance harness (`buffer ... is not owned by resource owner Portal`). |
| Threads? | **Not thread-safe, not reentrant.** | One reader belongs to one source in one backend, and a PostgreSQL backend is single-threaded. A concurrent reader would need its own synchronisation and would have to say so in the header. |
| Error reporting | `read_at`/`size` return −1; `last_error()` gives the reason. Never NULL, empty when nothing failed, valid until the next call. | No exception crosses into the C ABI, and the C side already has an errbuf convention. |

`FileReader` loops on short `pread()` until it returns 0, because `pread` may
return short for reasons that are not EOF (a signal, a large request on some
filesystems). Zero is the only thing that means end of object. It also refuses
anything that is not a regular file: a directory, a fifo or a device has no
stable size, and Parquet needs to read its footer from the end.

---

## 4. Cancellation

`ObjectReader` does not know about PostgreSQL. The interrupt check is
**injected**: `set_interrupt_hook()` installs a callback invoked *before each
physical read* — between range reads, never inside a `pread()` already in the
kernel.

The hook may `longjmp`, because that is what `ereport` does. So the reader holds
no C++ object needing destruction across the call: the hook runs first, then the
read, and the counters are touched only after the read returns.

The hook is `xpq_interrupt_check()`, installed once from the module's `_PG_init`
and consisting of `CHECK_FOR_INTERRUPTS()`. It is the only PostgreSQL-aware line
in the byte path, and it reaches the reader as a function pointer, so
`xpb_object_reader.cpp` includes no PostgreSQL header.

Measured, against the same scan run uninterrupted (§6): cancelled at 26–39 ms
against 283–297 ms, over four runs. The bound is calibrated against the
uninterrupted scan in the same run rather than a constant, because a fixed
threshold flaked between 110 ms idle and 170 ms busy earlier in this project.

---

## 5. Ownership and lifetime

### The map

| Object | Owned by | Lives until | Released by |
|---|---|---|---|
| OS file descriptor | `FileReader` | its destructor | `FileReader::close()`, idempotent |
| `ObjectReader` | `XpqReader::object` (`unique_ptr`) | `XpqReader` destruction | `xpq_close()` → `delete` |
| `ArrowObjectInput` | `XpqReader::input` (`shared_ptr`), Arrow may hold a reference | `XpqReader` destruction | borrows the `ObjectReader`, never owns it |
| `XpqReader` | the C side, as an opaque pointer | the source's memory context | `xpq_release()` or `xpq_holder_cleanup()` |
| decoded row group (`held`) | `XpqReader` | the next `xpq_read_row_group()` | replaced wholesale — this is the batch contract's borrow window |

Declaration order inside `XpqReader` is load-bearing and commented as such:
`object` is declared **before** `input`, so since members are destroyed in
reverse order, the adapter dies before the reader it borrows.

The reader is a C++ object holding an OS descriptor and is **not** in a
PostgreSQL memory context. An `ereport` anywhere in the pipeline longjmps past
`end()`. Two mechanisms cover that:

* `XpqSourceState.cb` — a `MemoryContextCallback` registered on the context the
  source was created in, calling `xpq_release()`. Embedded in the struct rather
  than separately allocated, because the context's callback list holds a
  *pointer* to it.
* `XpqReaderHolder.cb` — the same thing for the one reader that is **not**
  behind an `XpBatchSource`, `xpq_columns()`. Added this phase; see §7.

### Verified, one exit path at a time

`extension/xp_batch/test/object_reader.sh` §6. Every assertion is "no
descriptor afterwards", which a reader that never opened a file would also
satisfy — so the section opens with a **positive control**, sampled from outside
the backend while a scan is still running:

```
sampled mid-scan: 1 descriptor(s) on reg_buh.parquet
ok  the reader holds exactly one descriptor while scanning
ok  normal end                                        -- no descriptor held afterwards
ok  error inside xpq_open (truncated footer)          -- no descriptor held afterwards
ok  error after open (column absent from the file)    -- no descriptor held afterwards
ok  statement_timeout during the scan                 -- no descriptor held afterwards
ok  xpq_columns, successful call (holder release)      -- no descriptor held afterwards
ok  xpq_columns, open refused before any reader exists -- no descriptor held afterwards
ok  pg_cancel_backend was delivered during the scan
ok  pg_cancel_backend                                 -- no descriptor held afterwards
ok  after backend exit nothing on the host holds the file
```

Counted inside the **same** backend after each exit, by target under
`/proc/<pid>/fd`. Backend exit is the one case counted host-wide, because a
count taken after the process is gone proves nothing about the mechanism.

The scan used for the control is the provider path, which holds the reader
across many batches. `xpq_scan` materialises into a tuplestore and is gone by
the time it returns a row, so a cursor on it shows nothing.

**Not exercised:** the post-open `ERROR` window inside `xpq_columns`
(`CStringGetTextDatum`, `tuplestore_putvalues`). The holder is there because
those calls can `ereport`; reaching one would need fault injection. Stated as
reasoned-but-untested rather than claimed as covered. The first version of that
test step used a truncated file, which makes `xpq_open_held()` return NULL
before any reader exists — it asserted nothing and was relabelled.

---

## 6. Measurements

All on `benchmarks/02-batch-joins/data/reg_buh.parquet` — 10 000 000 rows, 200
row groups, 8 columns, snappy, Parquet v2.6, written with statistics.
PostgreSQL 20devel, `xp_batch` in `shared_preload_libraries`, Arrow/Parquet C++
25.0. Counters come from the `ObjectReader`, which is the only layer that sees a
physical read.

### Projection and pruning decide the physical reads

This is the architectural acceptance test.

| query | row groups | data reads | data bytes |
|---|---|---|---|
| full scan, 4 of 8 columns | 200 | 400 | 52 557 608 |
| pruned to `[25..36]`, 4 columns | 21 | 42 | 5 517 323 |
| `[25..36]`, 1 column | 21 | 21 | 4 538 |
| `[25..25]`, 1 column | 2 | 4 | — |
| **pruned to nothing** | **0** | **0** | **0** |

A pruned row group costs no physical read at all. The footer still costs 2
metadata reads in the pruned-to-nothing case: opening is real work and is
reported, not hidden.

`meta_calls` and `data_calls` are now **counted** in the reader, not derived
from byte totals. Before this phase the split was inferred.

### Requested versus returned

`bytes_requested` and `bytes_returned` are counted separately, and are **equal
in every case measured** (e.g. 5 755 016 for the four-column pruned scan). That
equality is the point: the over-read that coalescing causes is Arrow asking for
a *wider range*, not the reader returning less than it was asked for.

### Range coalescing is gap-based, with measurable over-read

Arrow's `CacheOptions::hole_size_limit` coalesces two wanted ranges into one
request when the gap between them is small enough. It is **gap-based**, not a
search for contiguous runs, and the bytes in the gap are fetched and thrown
away. Pruned to the 21 row groups of `[25..36]`:

| columns requested | data reads | over-read bytes |
|---|---|---|
| `period_key` | 21 | 0 |
| `+ company_key` (gap 2 933) | 21 | 61 593 |
| `+ credit_key` (gap 51 808) | 42 | 0 |

A small gap is coalesced and its bytes are paid for; a large gap is not
coalesced and nothing extra is fetched. "Contiguous runs" would be the wrong
term for this and is not used.

### Overhead of the boundary

"Before" is `a08893b` rebuilt and reinstalled; "after" is this branch. Same
file, same slice, same queries, 3 warm-ups then 11 timed repetitions, median
per repetition, three repetitions of the whole set per build.

**This is not an optimization phase and there is no performance claim here.**

| shape | before (3 medians, ms) | after (3 medians, ms) |
|---|---|---|
| full scan, 4 cols | 200.3 / 228.5 / 217.6 | 217.5 / 218.5 / 215.7 |
| projected, 1 col | 84.3 / 72.3 / 64.3 | 74.9 / 66.6 / 76.1 |
| pruned `[25..36]`, 4 cols | 53.5 / 38.5 / 44.9 | 53.4 / 40.6 / 42.8 |
| aggregate via operators | 74.3 / 50.2 / 57.4 | 50.3 / 53.0 / 51.3 |

The between-build difference is smaller than the within-build spread in every
shape. The aggregate row looks like a 74 → 50 ms improvement in the first pair
of runs; repeating "before" put it at 50.2 ms on its own, so that was noise.
**No overhead is resolvable at this harness's resolution**, and no improvement
is claimed either.

What *is* resolution-free is whether the boundary changed **which** ranges Arrow
asks for. Same five shapes, both builds:

| shape | read_calls | meta bytes | data bytes |
|---|---|---|---|
| full scan, 4 cols | 402 | 237 693 | 52 557 608 |
| projected, 1 col | 202 | 237 693 | 43 202 |
| pruned `[25..36]`, 4 cols | 44 | 237 693 | 5 517 323 |
| pruned `[25..25]`, 1 col | 4 | 237 693 | 437 |
| pruned to nothing | 2 | 237 693 | 0 |

Byte-for-byte and call-for-call **identical** before and after. The adapter did
not change Arrow's range selection. This is the stronger statement and it does
not depend on timing.

### Correctness

Against the PostgreSQL oracle, unchanged by this phase:

* benchmark 08 `check.py` — **postgres/pyarrow 14 agree, 0 differ; xpbatch
  provider 41 agree, 0 differ**;
* benchmark 02 gate — `correctness gate PASS`, `third-party gate PASS` (DuckDB
  over the same file);
* `pruning-control.py` — clustered 21/200 row groups read, shuffled 200/200,
  **same checksum from both**, so pruning is not changing the answer;
* the Parquet arm's checksum is `09e54f9a0108ff447c05f2cf63ca63da|200`, the same
  value as before the boundary existed.

`check.py` declared `xpq_scan` with the old 20-column signature and was updated
to 24. That staleness has broken runs three times in this project.

---

## 7. Defects found

### A leaked file descriptor on every bad file — pre-existing

`xpq_open()` held its `XpqReader` as a raw pointer declared inside the `try`
block. `parquet::ParquetFileReader::Open()` throws `ParquetException` on a
truncated or corrupt footer, and that throw lands in the `catch` handlers, which
cannot see a pointer declared in the `try`. Every bad file leaked the reader and
with it the open descriptor, for the life of the backend.

Found by §4 of the new test: 10 failed opens, 10 descriptors still held. The
same shape is present at `a08893b` with `arrow::io::ReadableFile`, so this is
**pre-existing, not introduced by this phase** — the earlier context-reset-
callback fix covered only the case where `xpq_open` *succeeded* and the scan
later errored.

Fixed by holding it in a `std::unique_ptr` and `release()`-ing on the success
path only. Test goes 14/1 → 15/0.

### `open_file_reader` could leak an fd if `new` threw

Between `::open()` and the `FileReader` constructor the descriptor is owned by
nothing. `operator new` can throw. Closed with an explicit `catch (...) { ::close(fd); throw; }`,
because this is the only place in the module that holds a descriptor with no
destructor behind it, and the caller's handler cannot see it.

### `xpq_columns()` had an unprotected open/close window

It opened a reader and closed it at the end of the function, with
`CStringGetTextDatum()` and `tuplestore_putvalues()` in between — both can
`ereport`, and the longjmp skipped the close. Given the same treatment as the
source path: one holder, one `MemoryContextCallback`, so both paths release a
reader the same way. Reachability is reasoned from the code, not demonstrated
(§5).

### Harness defects found and fixed in passing

* `optional_module.sh` renames the installed `.so` out of the way and restored
  it at the end of the script. Killed in between, it left the module hidden and
  **every later Parquet test failed with "could not access file"**, which reads
  as a product defect and is not one. It happened to this run. The restore is
  now a `trap ... EXIT HUP INT TERM`.
* `optional_module.sh` also asserted against benchmark 05's `reg2` /
  `dim_company_d` fixtures unconditionally; against a benchmark 02 database it
  reported three failures that were missing fixtures. It now picks the pipeline
  from what the database holds and skips with a reason when neither is there.
  Its zlfs arm hardcoded `[1..12]`, which has no built zone here — the slice now
  comes from `zlfs_zone_info()`, and the arm skips rather than building a zone
  into a benchmark database.
* `tail -1` landing on an ERROR's `HINT` line, making a refusal read as an
  answer — the **third** script in this suite with that bug. `optional_module.sh`
  now matches on the whole output.
* `object_reader.sh` §6 left its probe `psql` alive; closing the fifo did not
  always end it, and a leftover idle backend **blocks a smart shutdown** — which
  is how `regression_guards.sh`'s restart stalled this run. It now terminates
  the session and, failing that, the backend.
* `object_reader.sh` §6's first baseline used a cursor on `xpq_scan` and
  reported 0 descriptors, which would have made all seven assertions vacuous.
  Replaced with the mid-scan positive control in §5.

### Environment repairs, recorded because they are not code changes

* `testdb` held `xp_batch` version 1.0 with only **4** of the **20** functions
  the current extension script declares; individual tests had been patching in
  loose, non-member functions. Repaired by dropping the two conflicting loose
  functions and recreating the extension. Table data untouched, dataset verified
  at 10 000 000 rows after.
* `zlfs_persistence.sh` is hardcoded to `/root/pginstall` and a nested `su`,
  from commit `e9c92de`. It cannot run in this environment. Unchanged by this
  phase and reported as pre-existing, not fixed here — out of scope.
* `/tmp/pgdata20/zlfs` holds 260 zone files, 211 MB, mostly orphans from dropped
  test databases, each producing a `cannot validate schema ... skipping`
  warning. Pre-existing; not cleaned up, because cleanup was not requested.

---

## 8. S3 readiness — no S3 code

The question this phase has to answer is narrow: **can an `S3Reader` be added
without changing `ParquetReader` or any XPBatch operator?** Answered from the
code.

### What an S3 implementation would have to provide

Only the three virtuals. `size()` from a HEAD; `read_at()` from a ranged GET
(`Range: bytes=offset-offset+nbytes-1`); `close()` releasing the client handle.
Error strings via `fail()`. Accounting, interrupt hook and argument validation
are already in the base class and would be inherited unchanged.

### What would NOT have to change

* `ParquetReader` and `ArrowObjectInput` — they see `ObjectReader`, and
  `ArrowObjectInput` holds a bare `ObjectReader *`.
* Every XPBatch operator — nothing above the provider knows a byte source
  exists.
* Projection and row-group pruning — both are decided in
  `xpb_parquet_module.c` from footer statistics, strictly above the reader.
  §6 shows they already reduce the ranges requested; over a remote source they
  would reduce the ranges *requested over the network* by the same mechanism.
* The counters — `read_calls`, `bytes_requested`, `bytes_returned` and the
  meta/data split are in the base class and mean the same thing for a GET as
  for a `pread`. `bytes_requested` vs `bytes_returned` becomes *more* useful
  there, since over-read is paid for in transfer.

### What WOULD have to change, and is not pretended otherwise

* **Where the reader is chosen.** `xpq_open()` calls `xpb::open_file_reader()`
  directly. A second implementation needs a URI scheme decision at that one
  line — one site, but it does not exist yet and is not written.
* **Retry and timeout.** A ranged GET fails transiently in ways `pread` does
  not. The current contract says a short read means end of object; an S3 reader
  must retry internally and must not surface a truncated transfer as EOF, or a
  dropped connection becomes a silently short Parquet column. **This is the one
  semantic the current contract does not pin down for a remote source, and it
  is where a naive S3 implementation would produce a silent wrong answer.** It
  needs deciding before any S3 code is written, not during.
* **Latency shape.** `hole_size_limit` was tuned, by Arrow, for local storage.
  Over a network the economics of one large request versus many small ones
  differ by orders of magnitude. That is a tuning question above the reader and
  does not change the interface.
* **Credentials and configuration.** No home for them exists. Not designed
  here.
* **Cancellation granularity.** The hook fires before each physical read, which
  for a multi-second GET means a cancel waits for the request to return. An S3
  reader would want the check inside its own retry loop too. The hook mechanism
  supports that; nothing about it needs changing.

### Answer

**Yes** for `ParquetReader` and the XPBatch operators: they would not change.
The additions are confined to one new `ObjectReader` subclass plus a scheme
decision at the single `open_file_reader()` call site. The open design question
is not the interface but **retry-versus-EOF**, named above.

---

## 9. What is not closed

* **Retry-versus-EOF for a remote source** — §8. Named, not solved. Nothing in
  this phase constructs a remote reader, so nothing is exposed to it today.
* **The post-open `ERROR` window in `xpq_columns`** — holder present, path not
  exercised. Reasoned from the code. §5.
* **The multi-chunk fallback in the shim** remains unexercised code, as recorded
  in benchmark 08: Arrow returned one chunk per column per row group in every
  case measured.
* **No `prefetch`/`read_ranges` on the interface.** Deliberate: nothing
  exercises them. Adding them now would be a capability claim with no caller.

---

## 10. Verdict

**A — the boundary is correct and worth keeping.**

Grounds, all from this phase and all measured:

1. The reader object file names zero Arrow symbols, and its build rule cannot
   acquire an Arrow include path.
2. Projection and pruning reduce physical reads measurably, down to **0 reads
   and 0 bytes** for a query pruned to nothing, counted at the bottom layer
   rather than inferred.
3. `read_calls`, `meta_bytes` and `data_bytes` are **identical before and after**
   across five shapes: the boundary did not change Arrow's range selection.
4. No overhead is resolvable at this harness's resolution, and none is claimed.
5. Every exit path releases the descriptor, against a positive control that
   proves one was held.
6. The boundary's own test found a **pre-existing** descriptor leak on every bad
   file that three earlier Parquet phases did not.
7. S3 can be added without changing `ParquetReader` or the operators, with one
   named open question that is not an interface problem.

Not A because it is finished: §9 lists what is open. A because the layering is
real in the code, measured rather than asserted, and the one thing it had to
prove — that the layer above decides which bytes and the layer below only
fetches them — is visible in the read counts.
