# Source contract hardening v1

The previous phase concluded that XPBatch can take an external physical source
through the typed batch boundary, but that the contract was honoured by
convention in several places rather than enforced. This phase made it
obligatory, and the audit it started with found five defects, two of them silent
wrong answers reachable from ordinary SQL.

Base `670cc64`. Everything below is either enforced by code that is named, or
marked as not enforced.

## Contract

```
provider registration
      |
source create
      |
next_batch *            (until it returns false)
      |
rescan ?*               (only if the source declared it)
      |
end / ERROR cleanup
```

What the layers own, unchanged from `EXTERNAL_SOURCE_PROVIDER_CONTRACT.md` and
now with the enforcement named:

```
planner/executor owns:          provider owns:
  projection request              physical access
  predicate request               decode
  SQL semantics                   source pruning
                                  decoded-buffer lifetime
                                  source-local metrics
```

| rule | enforced by |
|---|---|
| the batch's columns are typed, and read through the declared type | `xpcb_i32()`/`xpcb_i64()` raise on a mismatch |
| a NULL is a bitmap bit, never a sentinel value | `xpcb_require_all_valid()` where no NULL branch exists |
| a borrowed buffer lives until the next `next_batch`/`rescan`/`end` | per-column `owns_data`; `xpcb_release_owned()` frees only what is owned |
| the predicate is the caller's unless the provider says otherwise | `XpbSourceProvider.filters_rows`, read at the one construction site |
| a source is rewound only if it says it can be | `xpcb_source_rescan()` |
| external resources are released exactly once, on every path | memory-context callback plus idempotent `end()` |
| no C++ exception crosses the C ABI | every shim entry point catches; tested by a deliberate throw |

## Provider ABI

```c
#define XPB_SOURCE_ABI_VERSION  0x58500001u

typedef struct XpbSourceProvider {
    uint32  abi_version;
    uint32  struct_size;
    const char *name;
    XpBatchSource *(*create)(const XpbSourceRequest *req);
    bool  (*describe)(...);
    /* ---- APPEND ONLY BELOW THIS LINE ---- */
    bool    filters_rows;
} XpbSourceProvider;
```

Version and size are read before any other field, because a descriptor from
another revision has its fields elsewhere. The registry zero-fills a local copy
and takes `struct_size` bytes, so a provider that predates an appended field
reads zero there rather than having memory past its own object read. A
`struct_size` below the fixed prefix, or above this build's `sizeof`, is refused
-- the second because those fields carry meanings this build cannot honour.

The version is not a small integer on purpose: offset 0 held `name` in every
earlier layout, so a distinctive constant is what makes a stale descriptor
detectable instead of plausible.

## Capabilities

One field, after asking which properties a caller actually varies on.

| capability | status | why |
|---|---|---|
| `supports_rescan` | **added** | `rescan` is implemented by all five sources and was called from one place that nothing calls; "the callback is non-NULL" stood in for "rescan works". pgcolumnar cannot rescan -- its `rescan` raises -- and had no way to say so |
| `filters_rows` | already present | the predicate is a request, and the two existing answers differed |
| `supports_projection` | not added | every source delivers exactly the requested columns; no reader would vary |
| `borrowed_buffers` | not added | already stated per column by `owns_data`, which is the granularity that matters since one batch mixes borrowed and owned columns |
| `supports_validity` | not added | what a consumer needs is enforcement on the batch in hand; a source that wrongly declared "no NULLs" would be believed |
| `physical_order_guarantee` | **not added, and not to be** | R1-11 was a statistic promoted into an ordering guarantee and it lost rows |

> Statistics, correlations and observed prefixes are never semantic
> capabilities. What a source may be trusted on is what it declares as a fact
> about the bytes it holds -- Parquet's row-group bounds are a number the writer
> committed to, and their absence means READ.

The zero value of every field is the conservative answer, so a source that does
not fill it in supports nothing.

## Column identity

R1-13 was a generic operator reading a ZLFS zone's columns positionally. The
audit for remaining instances of that class found, and this phase closed:

- **R1-15**, the attribute offset itself: `xpb_getattr_scalar()` aligned the
  attribute's LENGTH instead of the running offset, so padding before a
  fast-path column was never counted. Every key and aggregate read in
  XpGroupAgg2 goes through that function. `(bool, int4, int4, int4)` gave
  `sum = 193 259 687 116 800` against PostgreSQL's `11 519 175`. Fixed by
  delegating to `align_fetch_then_add()` and `fetch_att_noerr()` from
  `access/tupmacs.h` -- the helpers `heap_deform_tuple()` uses, and the ones
  `xpb_src_heap.c` already delegated to. Second site, same arithmetic, in
  `xpb_qual.c`'s `fixed_offset`, which the reachability map had carried as a
  known hole for two phases.
- benchmark SRFs still hardcode their schema (attnos and table names). That is
  the benchmark's own shape, called by name, and is left as is -- but
  `zlfs_group_sum` now checks the zone's `col_types[]` and `col_validity[]`
  before reading, since resolving columns by attno correctly and then reading
  them as `int32 *` regardless was how an int8 column came apart into halves.

Not done, and named rather than implied: a shared shape helper. The audit
proposed `xpb_cols_match(req, src_attnos, src_types, src_nullable)`; the
remaining call sites are three benchmark control arms and one fast path that
already checks inline, so a helper would today have one real user. It is the
right change when the second planner-installed fast path appears.

## Validity

The audit classified every consumer of `XpColumnBatch`. Which sources can emit
a bitmap at all:

| source | can emit | evidence |
|---|---|---|
| heap, fixed path | no | refuses a nullable layout at create |
| heap, deform/projected | yes | `xpcb_col_add_validity()` on the first NULL |
| pgcolumnar | yes | byte-aligned slice of the present bitmap |
| ZLFS zone | yes | the zone carries `col_validity[]` |
| Parquet | yes | Arrow's bitmap, borrowed as is |

Correction to the previous audit, which had recorded these as reachable through
the heap source: the three operators concerned construct the heap source's
FIXED path, which refuses nullable columns at create. The carriers are ZLFS
zones and pgcolumnar.

Who honours the bitmap: `xpb_typed_pipeline.c` throughout (join key, group keys,
sums, and all-NULL groups returning SQL NULL), both dimension builds and both
join keys in `xpb_v2_report.c`, `zlfs_build_zone`, the heap and pgcolumnar
predicate paths including the bit restored for a rejected row, and the Parquet
probe.

Who does not, and now refuses: `BatchHashJoin`, `BatchHashJoin2`,
`BatchGroupAgg`, `XpBatchPartition` and `zlfs_group_sum`. Measured before the
refusal, on a ZLFS zone over a nullable `company_key`:

| | groups | sum |
|---|---|---|
| PostgreSQL | 252 | 1 063 110 |
| `xpb_batch_groupby` | 240 | 1 063 110 |
| `zlfs_group_sum` | 240 | 1 063 110 |

The sums agreed. The NULL group key was merged into a value group, so twelve
groups -- one per period in the range -- vanished. A sum-only comparison passes
this defect.

**R1-14** is the same class on the predicate side: XpGroupAgg2's NULL scope
guard covered the grouping and aggregate columns and ran before the predicates
were extracted, so a predicate on a nullable column tested every NULL row as
zero and accepted it. 2400 groups against PostgreSQL's 1200. The path is
declined rather than taught NULL semantics, because it has no NULL branch
anywhere.

Known and not closed: the two join probes' gather paths set `validity = NULL`
while compacting values. With a bitmap refused at the top of each operator those
lines are unreachable rather than fixed; rebuilding a bitmap through a selection
vector is work for whoever implements NULL semantics there.

## Ownership and lifetime

A borrowed column lives until the next `next_batch()`, `rescan()` or `end()` on
that source. The Parquet provider satisfies this structurally: the shim holds one
row group and releases it before reading the next, so the contract's window and
the reader's window coincide by construction.

`end()` is not a guarantee -- an `ereport` above the provider longjmps past it --
so anything outside PostgreSQL's memory contexts is anchored to a context:

```c
st->cb.func = my_cleanup;  st->cb.arg = st;
MemoryContextRegisterResetCallback(CurrentMemoryContext, &st->cb);
```

Which makes **double release** the thing to get right, and it was not: calling
`end()` twice gave `buffer 0xffed is not owned by resource owner Portal`, the
heap source releasing its visibility-map pin and relation lock twice. Heap and
pgcolumnar now clear each handle as they release it; ZLFS holds nothing; Parquet
already funnelled every disposal through one `xpq_release()`. The conformance
harness calls `end()` twice on purpose.

Measured, not argued: ten failing queries in one session left ten open
descriptors on the Parquet file before the context callback existed, and zero
after.

## Rescan

`supports_rescan = true` means `rescan` works. `false` means the caller must not
rewind, and `xpcb_source_rescan()` refuses before calling through a pointer that
may be NULL.

heap, ZLFS and Parquet declare true; **pgcolumnar declares false**, which is the
capability's first real use. Append declares the AND of its children.

No operator rescans today -- the one caller is `append_rescan`, forwarding to
children, and nothing calls it. So this is a declaration backed by the
conformance harness (which rescans and compares row counts) rather than by a
production path. Reopening a source to emulate rescan is deliberately not done:
for pgcolumnar it would rebuild the projection from state the source does not
keep, and a second scan that silently differs from the first is worse than a
refusal.

## Cancellation

Arrow's call stack has no `CHECK_FOR_INTERRUPTS()`. Without one per row group a
scan could not be cancelled at all: `statement_timeout = 10 ms` took 389 ms to
fire on a 200-row-group file, and 26 ms after. The pipeline's batch loop checks
too, because the join and aggregate loops reach no interrupt point of their own.

Granularity is one row group and one batch -- 50 000 rows, not 10 000 000. For a
remote reader it would be whatever one request takes, which is why
`PARQUET_OBJECT_STORAGE.md` still lists uninterruptible I/O as a blocker there.

The test calibrates instead of using a constant: it measures the same scan
uninterrupted and requires cancellation to be a fraction of it. A fixed 150 ms
bound passed at 110 ms on an idle host and failed at 170 ms on a busy one, which
says nothing about interruptibility.

## C/C++ boundary

- No exception may cross the C ABI. `ereport`'s longjmp runs no C++ destructors,
  and a C++ exception propagating through C frames is equally unspecified.
- No PostgreSQL API from C++: the shim includes no PostgreSQL header, errors
  come back as a return code plus a message buffer.
- No PostgreSQL API from any thread a provider creates. Nothing here creates
  threads; a provider enabling a library's thread pool owns this rule.

Tested rather than argued. A malformed file does not exercise it -- Arrow reports
those as a Status -- so `xpq_selftest_throw(kind)` throws on purpose: a
`std::runtime_error`, a `parquet::ParquetException`, and an `int` that only
`catch (...)` can take. Each arrives in C as a return code, and the backend is
asserted usable afterwards. The status path is covered in the same test with
random bytes and with PAR1 magic around a garbage footer.

One guard was added during the audit: `xpq_row_group_rows()` was unguarded like
the trivial accessors around it, but `FileMetaData::RowGroup()` returns a
`unique_ptr` and therefore allocates, so a `std::bad_alloc` could have escaped.

## Registry semantics

| situation | behaviour | proved by |
|---|---|---|
| duplicate provider name | refused; load order never decides | `provider_negative.sh` case 4 |
| unknown provider | explicit error, no fallback to another source | case 5 |
| ABI version mismatch | refused, and absent from the registry afterwards | `provider_abi.sh` |
| descriptor shorter than the fixed prefix | refused before `name` or `create` is read | `provider_abi.sh` |
| descriptor shorter than the current struct | accepted; missing fields read as zero | `provider_abi.sh` |
| descriptor larger than this build knows | refused | `provider_abi.sh` |
| descriptor lifetime | the registry stores its own copy | by construction |
| library present, registered nothing | looks exactly like an unknown provider | `provider_negative.sh` case 7 |

**Unload is not supported, and nothing here pretends otherwise.** There is no
unregister. PostgreSQL does not unload a module once loaded, and the registry
depends on that in one place: `name` is a pointer into the provider module's
text, not a copy. Registration is append-only and per-backend, and is not
restricted to `_PG_init` -- the registry accepts it at any time -- so the
provider count is not stable within a session. Nothing depends on it being
stable.

## Built-in vs external providers

All four sources now go through one registry:

```
generic execution code
      |
provider lookup / source create
      |
heap / ZLFS / pgColumnar / Parquet
```

heap, ZLFS and pgcolumnar are **built-in**: linked into `xp_batch.so` and
registered from its own `_PG_init`. That is the distinction, and it is
deliberate -- heap has no optional dependency and the cluster must start with no
provider module present. Registering them makes the LOOKUP uniform, not the
dependency optional.

The audit's question was whether their direct wiring was legacy or a different
interface. It was wiring: all three already produced `XpColumnBatch` through
`XpBatchSourceOps`, and two took exactly the fields `XpbSourceRequest` carries.
The ZLFS adapter is the only one that does more than rearrange arguments -- it
performs the zone lookup each caller used to write inline.

`xpb_batch_join2_groupby`'s four construction branches are now one lookup. What
remains in it is the benchmark's own schema: which attnos to project, and which
relation holds the columnar copy. The other SRFs still have their `strcmp`
chains; they are benchmark entry points and migrating them is mechanical work
with no contract consequence.

## Conformance tests

`xpb_source_conformance(provider, relname, uri, attnos, colnames, lo, hi)` drives
any registered provider through the lifecycle and returns one row per check.
`test/source_conformance.sh` runs it for all four:

```
providers driven: 4
48 pass, 0 fail, 13 skip
```

Each skip names its reason and the test that does cover it: cancellation needs a
second session (`parquet_error_paths.sh`), ERROR cleanup needs an injected
failure (`provider_negative.sh`, ten modes), type-mismatch refusal depends on
the data (`provider_negative.sh`, with a file built for it). A report that hides
what it did not check is worth less than no report.

It earned itself twice over on the first run: it found the non-idempotent
`end()`, and it falsified my own `supports_rescan = true` for pgcolumnar.

## Test fixture isolation

Several C entry points resolve `reg_buh`, `dim_period` and `dim_account`
themselves, so tests that exercise them must build tables under those names.
`silent_drop_map.sh` took the database as an argument and dropped them in it,
which destroyed a 10M-row dataset during this phase. It now creates and drops
its own database, which is the pattern `heap_layout_guard.sh` already used.

Correction: `heap_layout_guard.sh` was not a second offender -- it has always
created its own database -- and `regression_guards.sh` uses `CREATE TEMP TABLE`,
shadowing through the temp schema. The exposure was one script and one bad
invocation.

`fixture_isolation_guard.sh` asserts the property rather than trusting each test:
census, run every test that touches the database, census again.

## Known limitations

1. The two join probes' gather paths drop a validity bitmap. Unreachable while
   the operators refuse bitmaps; not fixed.
2. No shared column-shape helper. One real user today; named as the right change
   when a second planner-installed fast path appears.
3. The benchmark SRFs other than `join2` still construct sources by `strcmp`.
   Mechanical, no contract consequence.
4. `supports_rescan` is declared by all four providers and exercised only by the
   conformance harness; no operator rescans.
5. `xp_page_summary` (the persistent page-summary side table) stores no record
   of which column a summary was built over, and `xpb_pageagg.c` uses a loaded
   summary as a summary of the current query's aggregate column. The audit could
   not find a writer for that table in the tree, so whether the mismatch is
   reachable is unresolved -- recorded here rather than claimed either way.
6. `xpb_pageagg.c` has no nullability gate and its own fast attribute fetch
   returns 0 for a NULL, so the audit expected R1-14's defect there too. **It
   did not reproduce.** With the node confirmed in the plan and the predicate
   pushed (`Pushed Pred: col1 < 10`):

   | query | PostgreSQL | XpPageAgg |
   |---|---|---|
   | `count(*) WHERE a < 10`, 1 in 7 rows NULL | 17 144 | 17 144 |
   | `sum(a)`, same table | 8 485 658 | 8 485 658 |
   | `sum(a)`, all 50 000 rows NULL | NULL | NULL |
   | `sum(a)`, empty table | NULL | NULL |

   So the NULL handling in that node is correct by some route the audit's
   reading missed, and the two claims that looked like defects -- NULL passing a
   predicate, and `sum` of nothing returning 0 -- are both wrong. Recorded as a
   negative result: the code reads as if it should fail and does not, which
   means the next person to touch it should find out why before relying on it.
7. `zlfs_persistence.sh` is hardcoded to paths that do not exist in this
   environment and has not run for several phases.
