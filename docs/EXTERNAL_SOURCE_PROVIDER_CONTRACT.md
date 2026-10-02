# External source provider — the contract

What a physical source outside `xp_batch.so` must do, what it may assume, and
what the layer above it owns. Written after the Parquet milestone, from the code
at `cbacd10` plus the fixes on this branch, and audited against it rather than
designed ahead of it.

Audience: whoever writes the second external provider. Everything here is either
enforced by code that is named, or marked as not enforced.

## Lifecycle

```
provider registration
      ↓
source create
      ↓
next_batch        (repeated until it returns false)
      ↓
rescan            (optional, returns to the first batch)
      ↓
end / ERROR cleanup
```

Who calls what, and what is guaranteed at each step:

| step | entry point | called by | guaranteed |
|---|---|---|---|
| registration | `xpb_register_source_provider()` | the provider's `_PG_init`, on `LOAD` | once per backend per module; refused on a mismatch |
| create | `XpbSourceProvider.create(req)` | the operator that needs a source | `req` is valid for the duration of the call only |
| next_batch | `ops->next_batch(src, batch)` | an operator loop | `batch->capacity` is set; the provider fills `nrows`, `ncols` and the columns |
| rescan | `ops->rescan(src)` | an operator that restarts | may be called before exhaustion |
| end | `ops->end(src)` | the operator, on the success path only | **may never be called** — see below |

`end()` is not a guarantee. An `ereport` anywhere above the provider longjmps
past it, which is why a provider holding anything outside PostgreSQL's memory
contexts must anchor it to a context:

```c
st->cb.func = my_cleanup;
st->cb.arg  = st;
MemoryContextRegisterResetCallback(CurrentMemoryContext, &st->cb);
```

The context to register on is the one `create()` was called in, which is the one
the provider's own state lives in. Measured consequence of not doing this: ten
failing queries in one session left ten open file descriptors on the Parquet
file, held for the life of the backend
(`test/parquet_error_paths.sh`). `end()` must then be idempotent, since both it
and the callback will run.

## Ownership

```
planner/executor owns:
  projection request
  predicate request
  SQL semantics

provider owns:
  physical access
  decode
  source pruning
  decoded-buffer lifetime
  source-local metrics
```

Three of those lines have teeth and are worth stating as rules.

**SQL semantics belong above the provider, including the predicate.** A predicate
in `XpbSourceRequest` is a REQUEST, not a delegation. A provider may apply it as
a row filter, or use it only to skip work, and it must say which through
`filters_rows`. `false` means the caller applies it. That field exists because
the two behaviours were indistinguishable and the difference is a wrong answer:
the heap and zlfs sources drop non-matching rows (`xpb_src_heap.c:329`), the
Parquet provider returns every row of every row group it reads.

**Decoded-buffer lifetime is the provider's, and the window is fixed.** A column
handed over with `owns_data = false` is borrowed, and the borrow lasts until the
next `next_batch()`, `rescan()` or `end()` on that source. The provider may
reuse the buffer after that and not before. The Parquet provider satisfies this
structurally: the shim holds one row group and releases it before reading the
next, so the contract's window and the reader's window are the same window by
construction rather than by care.

**Source-local metrics stay source-local.** A generic operator must not read
them. This is the line R1-13 crossed: `xpb_groupagg2.c` read a ZLFS zone's
columns positionally, which made a zone built for one query answer every query
over the same key range — `sum(payload)` returned the sum of `amount_dt`, out by
a factor of ten, silently.

## Capabilities

The properties the upper layer actually needs from a source. Listed because the
answer to "what should a source declare?" is otherwise unbounded, and because
two of these are currently assumed rather than declared.

| capability | who states it | enforced where |
|---|---|---|
| supported types | the batch column's `type`, per column, per batch | `xpcb_i32()`/`xpcb_i64()` raise on a mismatch |
| NULL validity | `validity == NULL` means all-valid; else bit set = valid | **not enforced**; see below |
| projection | the request's `attnos`/`colnames`; the provider refuses what it cannot deliver | `xpq_create()` refuses an unknown or repeated column |
| predicate/range support | `filters_rows` | read by the caller at `xpb_batch_hashjoin.c:826` |
| pruning | not declared at all — an internal optimisation, visible only in metrics | n/a |
| borrowed buffers | `owns_data`/`owns_validity`, per column | `xpcb_release_owned()` frees only what is owned |
| rescannable | assumed of every source; `ops->rescan` is not optional | not enforced |
| physical-order guarantee | **nothing declares this, and nothing may assume it** | see below |

Two gaps, stated rather than closed:

**Validity is declared per column but not honoured by every operator.**
`xpb_batch_hashjoin.c`, `xpb_batch_groupagg.c` and `xpb_batch_partition.c` never
call `xpcb_isnull()`. The join probes' gather paths set `validity = NULL` on the
output while copying values, so a NULL would become whatever the source left in
the data slot. `batch_range_select()` refuses a column with a validity bitmap
rather than add a fourth such path. Recorded in
`docs/roadmap/silent-drop-reachability.md`. A provider delivering nullable
columns today is relying on which operators it reaches.

**Rescan is assumed, not declared.** Every source implements `rescan`, so nothing
has yet needed a capability for "cannot be rescanned". A streaming source would.

### Statistics are not semantic guarantees

The one rule in this document that exists because of a specific history.

A source may hold statistics, correlations, declared bounds, sort hints. None of
them is a guarantee of anything, and in particular:

> No statistic may be promoted into an ordering guarantee, and no
> `guarantees_order` capability may be inferred from a correlation.

R1-11 is the reason. `XpGroupAgg2` inferred "the data is sorted" from a
correlation estimate and from the first page, and skipped the rest of a scan on
that basis: descending data returned 0 of 501 groups, near-sorted data lost
0.1–6.8% of rows with a correct group count. The fix was to delete the
inference, not to improve it.

What a source may be trusted on is what it **declares as a fact about the bytes
it holds**: Parquet row-group statistics are bounds the writer wrote down, so a
row group whose declared min/max cannot intersect the predicate holds no matching
row — and absence of statistics means READ, never "probably skip". That is the
whole admissible use, and the distinction is between a number a writer committed
to and a number someone estimated.

## The ABI, and what it refuses

`XpbSourceProvider` crosses a module boundary with nothing in the build system
rebuilding the two sides together, so the descriptor carries its own version and
size:

```c
#define XPB_SOURCE_ABI_VERSION  0x58500001u

typedef struct XpbSourceProvider {
    uint32  abi_version;    /* XPB_SOURCE_PROVIDER_HEADER sets both */
    uint32  struct_size;
    const char *name;
    XpBatchSource *(*create)(const XpbSourceRequest *req);
    bool  (*describe)(...);
    /* ── APPEND ONLY BELOW THIS LINE ── */
    bool    filters_rows;
} XpbSourceProvider;
```

Rules, each with the behaviour that enforces it:

- **Version first, size second, everything else after.** Both are read before any
  other field, because a descriptor from another revision has its fields
  elsewhere. A wrong version is refused.
- **Append only.** A field added below the marker is invisible to an older
  provider, which reports a smaller `struct_size` and reads zero there. A field
  inserted above it changes the meaning of every older provider's bytes and
  requires a version bump.
- **Size-aware copy.** The registry zero-fills a local descriptor and copies
  `struct_size` bytes, so a provider that predates an appended field never has
  memory past its own object read. `struct_size` below the fixed prefix or above
  this build's `sizeof` is refused — the latter because those fields carry
  meanings this build cannot honour.
- **The default of a missing field must be the safe direction.** `filters_rows`
  false means the caller applies the predicate, which costs a pass and changes no
  answer. The unsafe default would have been silently wrong output, and before
  the version fields existed, that is exactly what a stale provider produced:
  `filters_rows` read `true` out of the low bytes of an older descriptor's
  `describe` pointer.
- **Duplicate names are refused**, not resolved by load order.
- **Registration is append-only and per-backend.** There is no unregister. The
  stored descriptor is a copy, so it does not depend on the provider's static
  object; `name` is still borrowed from the provider's text, which is safe only
  because PostgreSQL never unloads a module. Registration is not restricted to
  `_PG_init` — the registry accepts it at any time — so provider count is not
  stable within a session. Nothing depends on it being stable.

All of this is asserted by `test/provider_abi.sh` (10 checks) and
`test/provider_negative.sh` (12), the latter covering ten failure modes against
four invariants: no silent fallback, no wrong result, no leak, no hang.

## The C++ / exception / thread boundary

For a provider whose backing library is C++, as Parquet's is:

- **No exception may cross the C ABI.** Undefined behaviour, not a caught error.
  Every shim entry point that can throw has a `try`/`catch`; the ones without are
  the ones that bounds-check and then read parsed structures. `xpq_row_group_rows`
  was moved into the first group during this audit because
  `FileMetaData::RowGroup()` returns a `unique_ptr` and therefore allocates.
- **No PostgreSQL API from C++.** The shim includes no PostgreSQL header. Errors
  come back as a return code plus a message buffer, and the C side raises them.
  This is what keeps `ereport`'s longjmp out of C++ stack frames with destructors
  pending.
- **No PostgreSQL API from any thread the provider creates.** A backend is
  single-threaded and `ereport` from another thread is undefined. Nothing here
  creates threads; a provider that enables a library's thread pool owns this
  rule.
- **Interrupts are the provider's responsibility while it is inside the
  library.** Arrow's call stack has no `CHECK_FOR_INTERRUPTS()`, so without one
  per row group a scan could not be cancelled: `statement_timeout = 10 ms` took
  389 ms to fire before, 26 ms after. The granularity a provider can offer is one
  unit of physical work; it must offer that much.

## Verdict

> Did Parquet validate XPBatch as a general external source substrate?
>
> **B — yes, but one small general contract change was needed.**

Argued from the code:

**Yes**, in the part that matters. A format with its own library, its own C++
dependency and its own optional packaging reached the existing operators through
`XpBatchSource` and produced the benchmark's checksum with no operator change:
`xpb_batch_join2_groupby`'s registry branch names no format, and `xp_batch.so`
contains no Arrow or Parquet reference (asserted by symbol inspection in
`test/optional_module.sh`). The join, aggregate and emit stages did not move when
the source changed. The batch contract — typed dense columns, set-bit-means-valid
validity, per-column ownership, a borrow window tied to `next_batch()` — needed
nothing added for a format it was not designed against.

**But one general change was required**, and it was not plumbing:
`filters_rows`. `XpbSourceRequest`'s predicate did not say who applies it, and
the two existing answers differed. That is a contract hole, not a Parquet
quirk — any pruning-only source would have hit it — and the fix belongs in the
general contract, which is where it went.

**Not C**, but the boundary is not clean either, and the difference matters:

- Only Parquet goes through the registry. ZLFS and pgcolumnar are still
  constructed by name and linked into `xp_batch.so`
  (`xpb_batch_hashjoin.c:30-35`, `Makefile`). That is pre-existing hardcoding,
  deliberately not migrated in this series.
- The `ext:<provider>:<uri>` entry point exists in one benchmark function. There
  is no planner path to an external provider, so "the planner owns the projection
  request" is true of the design and not yet exercised by a planner.
- R1-13 was a generic operator reaching into a provider's private structure, and
  it was found by this audit rather than by the contract. The contract did not
  prevent it because `ZlfsZone` is a struct in a header that `xpb_groupagg2.c`
  includes, not something reached through `XpBatchSource`. A boundary that can be
  bypassed by an `#include` is a convention, not an enforcement.

So: the ABI is sufficient for an external provider, with the one field added. The
leakage that remains is in the operators that bypass the ABI, not in the ABI.

## Next justified step

`ObjectReader` for the local file, no S3: make the Parquet reader stop depending
on a POSIX file API directly.

```
Parquet provider
     |
ParquetReader
     |
ObjectReader
     |
  FileReader
```

Justified by this audit rather than by ambition: `xpq_open()` constructs the local
reader inside the C++ half (`docs/PARQUET_OBJECT_STORAGE.md`, blocker 1), which is
the one structural thing between the current provider and a byte-range backend.
Everything else on that list is policy.

Not justified yet, and not to be started before the above: S3, async or prefetch,
a planner cost model for sources, automatic source choice, further Parquet
optimisation, broader type coverage.
