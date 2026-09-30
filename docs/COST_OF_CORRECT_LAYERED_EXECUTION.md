# Cost of Correct Layered Execution

Starting point `a77b73f`. Measurement only: nothing here was tuned, and no
optimisation was made in response to anything found.

## Question

After the layered executor learned to give the right answer or refuse the
query, what exactly do we now pay time for?

Not "path X beats path Y" — which layer becomes dominant, for which kind of
work, and why.

## Experimental setup

One host, one server, one dataset, all cells back to back. PostgreSQL 20devel,
pgcolumnar 1.0-alpha5, `work_mem=64MB`, `jit=off`,
`max_parallel_workers_per_gather=0`, `XPCB_BATCH_CAP=65536`.

Every cell runs in **one session**: rep 1 is kept and labelled cold, reps 2..11
are the warm sample, median reported. Session scope is deliberate — the ZLFS
registry is built per backend, so a per-connection loop charges every run for a
directory scan a real workload pays once. Both numbers are reported.

`xp_batch` is in `shared_preload_libraries`, so every rebuild here was followed
by `make install` **and a server restart**; without the restart the postmaster
serves the previous `.so` and the measurement is of the old code.

Runners: `benchmarks/06-cost-of-correct-execution/run-cost.sh` (ladders A–C)
and `run-boundaries.sh` (join hit rate, refusal cost). Raw output under
`raw/`, not rewritten after the fact.

### Correctness gate

**51 of 51 cells match PostgreSQL**, checked before any timing is taken. The
oracle is the same report written as plain SQL against the same fact table and
the same two dimension tables, with a checksum over every grouping key and
every aggregate, sorted before hashing so output order cannot enter it.

A/B agreement between two Xp arms is not accepted as correctness. AGG_CAP and
S2CAP showed two arms dropping the same rows with the equivalence gate green;
PostgreSQL is a mandatory third party.

That rule earned its keep twice in this phase. `run-boundaries.sh` first
reported a mismatch on a shape that was in fact correct — it hashed unsorted
psql output, and `XpGroupAgg2` emits groups in hash order while `HashAggregate`
does not. The harness was wrong, not the node. An order-independent checksum
was already the rule; this script had not followed it.

## Execution paths

The ladder is split across two tables, and that is not cosmetic:

| ladder | table | paths |
|---|---|---|
| A | `reg2_fixed` | fixed, deform, projected, projected+early |
| B | `reg2` / `reg2_col` / ZLFS zone | deform, projected, early, pgColumnar, ZLFS |
| C | `reg2_card`, `reg2_card2`, `reg2_dim` | group cardinality |

`reg2` carries a varlena at attnum 2, ahead of every column the pipeline reads,
so the fixed-offset path **cannot address it**. `fixed` does not exist on
`reg2`, and the dataset was not reshaped to manufacture a full table. Ladder A
therefore answers the four-way heap question on one table; ladder B answers the
storage question on another. Rows are the same count (1,000,008) in both.

## Instrumentation

Added in this phase, all observational — no semantics changed, no path
selection changed, nothing allocated in a hot loop:

- `emit` — the tuplestore fill, previously untimed **and outside `total`**.
- `flow_rows_in / _j1 / _j2 / _agg_updates / _rows_emitted / _emit_slots_scanned`
  — the row-flow model. The two survivor counts are taken in their own passes,
  outside the timed loops, so stage timings stay comparable with the
  uninstrumented binary.
- `pages_scanned`, `pages_rejected`, `tuples_visited`, `tuples_passed` —
  already collected, never printed. `tuples_visited` is the only **pre**-predicate
  row count for a heap path: `rows` is post-predicate, because every heap path
  filters inside `next_batch`.
- `groups_copied`, `bytes_copied` for pgColumnar — separates the zero-copy
  borrow from the materialise-and-filter walk.
- `zlfs_scan_dir_ms`, `zone_rows`, `zone_bytes`, `zone_ncols` — splits ZLFS's
  `open`, which is where all of its query-time cost turned out to live.

### Instrumentation overhead

Measured, paired: `a77b73f` binary against the instrumented binary, rebuild +
install + restart between, 20 warm reps each in one session.

| path | uninstrumented median | IQR | instrumented median | IQR | ratio |
|---|---|---|---|---|---|
| heap-deform | 89.5 | [88–91] | 87.8 | [81–100] | 0.98 |
| heap-projected | 93.0 | [90–95] | 96.5 | [92–103] | 1.04 |
| heap-early | 88.4 | [87–94] | 90.1 | [88–97] | 1.02 |
| pgcolumnar | 42.8 | [42–44] | 43.2 | [41–52] | 1.01 |
| zlfs | 13.3 | [12–16] | 12.2 | [11–15] | 0.92 |

Ratios straddle 1.0 in both directions and the instrumented IQRs are wider than
the differences. **No measurable instrumentation overhead on this host.** Not
"zero" — below what this pairing can resolve.

## Row-flow model

Full range, `reg2`, heap-deform:

```
1,000,008  tuples_visited        (pre-predicate, from the scan)
1,000,008  rows_in               (post-predicate; predicate is 100% here)
1,000,008  rows_j1               (dim1 hit)
1,000,008  rows_j2 = agg_updates (dim2 hit)
      200  groups
      200  rows_emitted          out of 16,384 slots swept
```

At `[90..91]` the same scan visits 1,000,008 tuples and 0 survive — which is
what makes that column the cleanest measurement in the set.

## Measurements

### Stage split, ms (warm), ladder A — `reg2_fixed`, four paths, one table

| path | range | total | source | join1 | join2 | agg | emit |
|---|---|---|---|---|---|---|---|
| fixed | 1..12 | 44.8 | 32.3 | 3.3 | 2.9 | 4.4 | 0.1 |
| fixed | 1..1 | 19.1 | 17.5 | 0.4 | 0.3 | 0.6 | 0.1 |
| fixed | 90..91 | 17.4 | **17.1** | 0.0 | 0.0 | 0.0 | 0.0 |
| deform | 1..12 | 82.2 | 70.6 | 3.5 | 2.4 | 4.8 | 0.1 |
| deform | 1..1 | 43.0 | 41.7 | 0.4 | 0.3 | 0.4 | 0.1 |
| deform | 90..91 | 56.7 | **56.4** | 0.0 | 0.0 | 0.0 | 0.1 |
| projected | 1..12 | 67.6 | 56.6 | 3.3 | 2.6 | 4.3 | 0.1 |
| projected | 1..1 | 55.6 | 54.1 | 0.4 | 0.3 | 0.4 | 0.1 |
| projected | 90..91 | 52.6 | **52.3** | 0.0 | 0.0 | 0.0 | 0.1 |
| early | 1..12 | 73.8 | 61.7 | 3.8 | 2.9 | 4.4 | 0.1 |
| early | 1..1 | 53.3 | 50.9 | 0.7 | 0.5 | 0.7 | 0.1 |
| early | 90..91 | 38.1 | **37.7** | 0.0 | 0.0 | 0.0 | 0.1 |

### Stage split, ms (warm), ladder B — storage paths

| path | range | total | source | operators | emit |
|---|---|---|---|---|---|
| heap-deform | 1..12 | 118.1 | 101.1 | 15.6 | 0.1 |
| heap-projected | 1..12 | 91.3 | 79.4 | 10.9 | 0.1 |
| heap-early | 1..12 | 99.3 | 85.6 | 12.6 | 0.1 |
| pgcolumnar | 1..12 | 41.3 | 30.1 | 10.5 | 0.1 |
| zlfs | 1..12 | 13.7 | **0.0** | 12.5 | 0.0 |
| pgcolumnar | 1..1 | 7.9 | 7.1 | 0.8 | 0.0 |
| zlfs | 1..1 | 1.3 | 0.0 | 1.0 | 0.1 |
| pgcolumnar | 90..91 | **0.2** | 0.0 | 0.0 | 0.0 |

### Group cardinality, `[1..12]`

| mode | groups | total | source | join1 | join2 | agg | emit | of which rehash | growths |
|---|---|---|---|---|---|---|---|---|---|
| heap-deform | 200 | 80.9 | 69.7 | 3.2 | 2.5 | 4.7 | 0.1 | 0.00 | 0 |
| card | 11,776 | 38.5 | 22.5 | 3.6 | 2.4 | 9.0 | 1.2 | 0.48 | 1 |
| dimgrow | 12,288 | 41.8 | 23.8 | 2.8 | 2.5 | 11.4 | 1.2 | 0.57 | 1 |
| card2 | 147,456 | 88.0 | 29.8 | 5.4 | 4.0 | 47.2 | 14.1 | 22.09 | 5 |
| card-zlfs | 11,776 | 15.3 | **0.0** | 2.9 | 2.4 | 9.2 | 1.2 | 0.49 | 1 |
| card2-zlfs | 147,456 | 44.2 | **0.0** | 3.9 | 2.8 | 36.5 | 14.2 | 18.84 | 5 |

### Join hit rate — `reg2 [1..12]`, heap-deform

Dimension rows deleted inside a transaction that is rolled back; the fact table
is never touched and the committed dataset is unchanged afterwards (verified:
`dim_company` back to 50 rows). The counters report the **achieved** rate, which
is not the intended one — `dim_company` holds 50 keys, so the intended
50/10/1% deletions landed elsewhere.

| achieved hit rate | rows_j1 | groups | source | join1 | join2 | agg | operators |
|---|---|---|---|---|---|---|---|
| 100% | 1,000,008 | 200 | 84.8 | 3.3 | 2.7 | 5.1 | 11.0 |
| 98% | 980,016 | 196 | 78.6 | 3.6 | 3.0 | 4.5 | 11.1 |
| 18% | 180,036 | 36 | 77.1 | 3.4 | 1.8 | 1.7 | 6.9 |
| 0% | 0 | 0 | 75.5 | 3.4 | 0.6 | 1.1 | 5.2 |

### Refusal cost — 2,000,000 rows, 1,000 groups

Three independent runs of the whole script, each a median of 9 reps. Run-to-run
spread on the refused shapes reaches 20%, so the range is given rather than a
single figure; the accepted shape is the stable one.

| shape | planner decision | three runs (ms) | result |
|---|---|---|---|
| fully supported | accepted: `Custom Scan (XpGroupAgg2)` | **26.5 / 30.9 / 31.9** | ok |
| one residual qual | refused → `HashAggregate` | **94.3 / 102.1 / 116.5** | ok |
| unsupported target expression | refused → `Aggregate` | 85.5 / 89.6 / 94.3 | ok |
| `HAVING` | refused → `HashAggregate` | 158.8 / 173.6 / 182.7 | ok |
| supported single aggregate | refused → `Aggregate` | 82.7 / 84.3 / 94.0 | ok |

## Per-stage costs

### The decode tax, isolated

At `[90..91]` the predicate rejects every row, so nothing reaches join, aggregate
or emit, and **every millisecond of `source` is scan plus decode**. Same table,
same 1,000,008 tuples visited, same visibility checks:

| path | source | vs fixed | attrs deformed | attrs walked | attrs materialized |
|---|---|---|---|---|---|
| fixed | 17.1 | — | 0 | 0 | 0 |
| deform | 56.4 | **+39.3** | 9,000,072 | 0 | 0 |
| projected | 52.3 | +35.2 | 0 | 5,000,040 | 5,000,040 |
| early | 37.7 | +20.6 | 0 | 1,000,008 | 1,000,008 |

17.1 ms is the irreducible floor: read 92 MB of heap, check visibility, load one
`int32` and compare it. Everything above it is decode.

`heap_deform_tuple` costs **+39.3 ms per 1,000,008 tuples at 9 attributes** —
about 4.4 ns per attribute, and 3.3× the entire fixed-offset scan. The batch
asked for 5 attributes and the generic path deformed 9.

### What each step in the ladder actually does

| step | Δsource @100% | Δsource @0% | mechanism |
|---|---|---|---|
| deform → projected | −14.0 ms | −4.1 ms | `CHEAPER_WORK` — 5 attributes materialised instead of 9 deformed |
| projected → early | **+5.1 ms** | −14.6 ms | `EARLIER_REJECTION` — the walk is abandoned at the predicate column |

These separate cleanly and they do not mix:

- **projected is selectivity-independent by construction.** It walks and
  materialises every tuple, then applies the predicate. Measured source across
  100/50/25/8.3/0% selectivity: 56.6, 56.3, 62.0, 54.1, 52.3 — flat within
  noise, as the mechanism predicts.
- **early is selectivity-dependent**, and at 100% selectivity it is *worse* than
  projected by 5.1 ms: the per-column predicate branch is paid on every tuple and
  nothing is ever rejected. It turns profitable somewhere around 50% selectivity
  and wins by 14.6 ms when everything is rejected.
- **deform also falls with selectivity** (70.6 → 41.7), which is easy to
  misread. It is not early rejection: deform applies the predicate immediately
  after `heap_deform_tuple` and so skips only *materialisation* for rejected
  rows. The deform itself is always paid in full — which is why deform at 0%
  selectivity (56.4) is still 3.3× fixed.

### The operator layer is source-independent

At 200 groups, full range, across sources whose `source` ranges from 101.1 ms to
0.0 ms:

| path | source | operators | emit |
|---|---|---|---|
| fixed | 32.3 | 10.6 | 0.1 |
| heap-deform | 101.1 | 15.6 | 0.1 |
| pgcolumnar | 30.1 | 10.5 | 0.1 |
| zlfs | **0.0** | 12.5 | 0.0 |

Join + aggregate is 10.5–15.6 ms for 1,000,008 rows into 200 groups whatever fed
it. ZLFS's entire query time *is* this number.

### pgColumnar: pruning separated from execution

| range | total | source | rowgroups read | rows in read groups | vec skipped | emitted | groups copied | bytes copied |
|---|---|---|---|---|---|---|---|---|
| 1..12 | 41.3 | 30.1 | **7 of 7** | 1,000,008 | 0 | 1,000,008 | **0** | **0** |
| 1..6 | 26.4 | 20.6 | 4 of 7 | 600,000 | 90,000 | 500,004 | 1 | 1,600,128 |
| 1..3 | 15.6 | 12.9 | 2 of 7 | 300,000 | 40,000 | 250,002 | 1 | 3,200,064 |
| 1..1 | 7.9 | 7.1 | 1 of 7 | 150,000 | 60,000 | 83,334 | 1 | 2,666,688 |
| 90..91 | **0.2** | 0.0 | **0 of 7** | 0 | 0 | 0 | 0 | 0 |

Both mechanisms are present and they are separable:

- At `[1..12]` **nothing is pruned** — 7 of 7 row groups read, 0 rows vector-skipped,
  0 bytes copied (pure zero-copy borrow). Source is 30.1 ms against heap-deform's
  101.1 ms on the same 1,000,008 rows. That 3.4× is a real layout and decode
  advantage, `BETTER_LAYOUT` + `CHEAPER_WORK`, with no pruning in it at all.
- At `[90..91]` the total is 0.2 ms because `rowgroups_read = 0`. This is
  `STORAGE_PRUNING`, and it must not be read as executor speed: it is the
  **absence of execution**. Comparing 0.2 ms against heap's 45.1 ms would be
  comparing a query that ran with one that did not.
- The middle rows show the switch from borrow to copy: as soon as the predicate
  rejects anything inside a read group, the group is materialised and rechecked
  (1 group copied, up to 3.2 MB).

### ZLFS: where the speed comes from

`source` is **0.0 ms at every range**. ZLFS does no read and no decode at query
time. The cost did not disappear; it moved, to two places the query timer does
not show:

| | cost | scales with |
|---|---|---|
| zone build | 147 ms for 1,000,008 rows, 19.5 MB | the data, once per zone |
| registry scan, **per backend** | **109.5 ms** | the whole zone directory — 180 files, 177 MB |
| per query | 0.0 ms source + 12.5 ms operators | the query |

Cold first call in a fresh backend: total 120.9 ms, `open` 109.5 ms of which
`zlfs_scan_dir_ms` 109.485. Warm in the same session: 14.4 ms. That is the
8.4× cold/warm ratio, and it is entirely the registry.

So ZLFS's classification is `BETTER_LAYOUT` plus work *moved out of the query*,
amortised per backend — not cheaper execution. Its operator cost is everybody
else's operator cost.

Two honest caveats on that 109.5 ms. It is proportional to the zone directory,
which currently holds 177 orphan files from dropped relations, each producing a
`cannot validate schema … skipping` warning. It is therefore an upper bound for
a clean installation and a lower bound for one that keeps accumulating. The
directory was left as it is: cleaning it would have changed the architectural
point being measured, and old benchmark artifacts are explicitly not to be
tidied.

### What refusal costs, and what that says about coverage

The first two rows are the only directly comparable pair: same table, same
predicate, same 1,000 groups, differing by one conjunct — `AND tag = 'keep'` —
which makes the query select strictly *fewer* rows. Accepted it costs
26.5–31.9 ms; refused it costs 94.3–116.5 ms. **Refusal is 3.0–4.4× on this
pair**, on a query that does less work.

That is the price of the Silent-Drop closure, and it is the right price: the
same query before the closure returned a wrong answer quickly. But it also
bounds what extending semantic coverage would be worth. Supporting one residual
conjunct inside `XpGroupAgg2` would move this query from ~100 ms to ~30 ms —
if and only if the node is selected at all, which is the next paragraph's
problem.

The last row is the useful negative result. `SELECT sum(v) FROM rf_t WHERE
k1 <= 500` is exactly the shape `XpPageAgg` accepts — one bare `Aggref`, one
pushable `int4` predicate, no `HAVING` — and it is still refused. Nothing is
missing semantically. So for this shape the gap is not coverage, and building
coverage would not close it.

### Normalized costs, `[1..12]`

| path | ns / source row | ns / agg update | µs / output group |
|---|---|---|---|
| fixed | 44.8 | 4.4 | 224 |
| deform (`reg2_fixed`) | 82.2 | 4.8 | 411 |
| projected | 67.6 | 4.3 | 338 |
| early | 73.8 | 4.4 | 369 |
| heap-deform (`reg2`) | 118.1 | 7.5 | 590 |
| pgcolumnar | 41.3 | 4.9 | 206 |
| zlfs | **13.7** | 5.3 | 68 |

`ns/agg update` is flat at 4.3–5.3 across every source — 7.5 for `reg2`
heap-deform, the widest row. The per-row aggregate cost is not where the paths
differ; `ns/source row` is, and it spans 9×.

## Interpretation

```
large scan, low group count, heap source:
    tuple decode dominates                    (source 70-86% of total)

selective predicate, heap source:
    early rejection dominates the difference  (but only below ~50% selectivity)

columnar selective scan:
    storage pruning dominates                 (and at 0% there is no execution)

columnar full scan:
    decode still dominates, but 3.4x cheaper  (no pruning involved)

pre-materialized source (ZLFS):
    the operator layer is the whole query

high group cardinality, any source:
    aggregate hash + result emission dominate

unsupported semantics:
    fallback dominates                        (3.0-4.4x on the comparable pair)
```

These came out of the measurements; none of them was assumed beforehand.

## What changed from Benchmark 05

- 05 measured stages; it had no row-flow accounting and no emit timer. `emit`
  was invisible **and excluded from `total_ms`** — harmless at 200 groups
  (0.1 ms), 14.2 ms at 147,456 groups, which is 32% of that cell's reported
  total. Reported figures for high-cardinality cells in 05 understate the query
  by roughly that much.
- 05-C put fixed at 32.1 ms against deform at 69.5 ms on `reg2_fixed`. This run
  reproduces that shape (32.3 / 70.6) and adds the part 05 could not separate:
  at 0% selectivity the same gap is 17.1 / 56.4, which is the decode tax with
  materialisation and aggregation removed.
- 05-A's ZLFS arm reported ~0 ms source and left it there. The registry scan
  behind it is now measured: 109.5 ms per backend.
- 05 compared pgColumnar and heap at ranges where pruning and decode were mixed.
  They are now separated, and at `[1..12]` — where pgColumnar prunes nothing —
  the 3.4× source advantage stands on its own.

## Current bottleneck

**There is no single bottleneck. Which layer dominates is a function of the
workload, and the measurements identify two distinct regimes.**

1. **Heap sources, moderate group counts — tuple decode.** `source` is 70–86%
   of total, and within it the decode tax above the scan floor is 20–39 ms per
   million rows. The fixed-offset path shows what the floor is (17.1 ms) and
   the generic path shows what is paid above it (+39.3 ms). This is the largest
   single removable cost in the set, and it is unchanged as the answer from 05.

2. **Pre-materialised or pruned sources, high group cardinality — the aggregate
   hash and result emission.** `card2-zlfs` is the clean case: source 0.0,
   aggregate 36.5 ms of which **18.8 ms is rehash** across 5 growths, and emit
   14.2 ms. With the source removed, aggregate + emit is 50.7 ms of the 58.4 ms
   the query actually takes (`total` 44.2 plus the 14.2 that `total` excludes) —
   **87%** in the group hash and the emission sweep.

Regime 2 is the genuinely new finding of this phase. It was not visible before
because capacity ceilings previously capped group counts — `card2` at 147,456
groups is only measurable at all because the fixed-capacity defects were closed.

Within regime 2, two costs are named but **not** acted on, per the rule that
measurement does not optimise:

- rehash is 43% of aggregation at 147,456 groups (18.8 of 36.5 ms, 5 growths
  from an initial 16,384 to 524,288).
- emit sweeps `grp_capacity`, not `grp_ngroups` — 524,288 slots to emit
  147,456 rows.

## What is not a bottleneck any more

- **Dimension hash probing.** `dim1_probes_per_lookup` and
  `dim2_probes_per_lookup` are 1.0000 lifetime in every cell, with
  `max_probe_lifetime` of 1. join1 + join2 is 5–9 ms per million rows and does
  not move with cardinality.
- **The per-row aggregate update.** Flat at 4.3–5.3 ns across every source.
- **Capacity handling as a correctness cost.** Growth now shows up as rehash
  time in a counter, which is a performance line item. It is no longer mixed
  with capacity correctness — nothing drops silently, so the cost of growth can
  be discussed on its own.
- **Instrumentation.** Below the noise floor of this host.

## Limitations

- **Projection width was not varied.** `xpb_v2_register_report` hard-codes a
  5-column projection; adding a width knob means changing the function's
  signature, which is a code change outside a measurement phase. The axis is
  unmeasured, not measured-and-flat.
- **Selectivity granularity is 1/12.** The predicate is a period range over 12
  periods, so 100 / 50 / 25 / 8.3 / 0% are reachable and 1% / 0.1% are not.
  The early-predicate crossover is therefore located "around 50%", not more
  precisely.
- **Join hit rate landed on 100 / 98 / 18 / 0%**, not the intended
  100 / 50 / 10 / 1%: `dim_company` holds 50 keys and the modulo deletions did
  not cut where intended. The counters report what was achieved; the axis is
  exercised but coarsely.
- **`total_ms` still excludes `emit`** — deliberately, since `total_ms` is
  written into every emitted row and cannot include the cost of emitting them.
  Every total in this document is therefore query time *without* emission;
  emit is listed separately in every table.
- **One host, one dataset shape, one `BLCKSZ`.** Cross-session comparison on
  this host drifts up to 1.6×; nothing here is compared across sessions.
- **The `[1..1]` projected cell is noisy** (54.1 ms against 62.0 at `[1..3]`).
  The mechanism says it should be flat and four of five points agree; the
  outlier is reported rather than smoothed.

## Found, not acted on

`XpGroupAgg2` is cost-gated in a way that depends on **key correlation, not
just cardinality**. The same query over 1,000 groups in 2,000,000 rows is
accepted when the key ascends with physical order (`k1 = g/2000`) and refused
when it does not (`k1 = g%1000`), at every table size tried from 10,000 to
2,000,000 rows and with parallelism and JIT both off. The first attempt at the
refusal-cost measurement used the uncorrelated form and got "refused" for all
five shapes — including the fully supported one — which would have measured
nothing about refusal at all.

`SELECT sum(v) FROM rf_t WHERE k1 <= 500` is likewise refused (→ `Aggregate`)
although it is exactly the shape `XpPageAgg` accepts: one bare `Aggref`, one
pushable predicate. Semantic coverage exists; the cost model does not select it
here.

Recorded as a symptom. Not investigated, not changed.

## Next decision

The data justifies one experiment, and it is not in the source layer.

Regime 2 is new, it is where the pipeline now spends its time once the source is
cheap, and its two costs are both measured and both structural: rehash at 43% of
aggregation, and an emission sweep over capacity rather than occupancy. Neither
is a tuning question — the first is the growth policy's shape, the second is the
group table's iteration contract.

Regime 1 is the larger absolute number but it is not a new finding, and the next
step there (a decode path that beats `projected+early`) is an optimisation, which
this phase is not permitted to start and which the data does not make more urgent
than it was after 05.

Proposed next question: **at high group cardinality, what is the cost structure
of the group hash itself** — growth policy, probe behaviour under real key
distributions, and the emission contract — measured the same way, with the source
held at zero by ZLFS so nothing else moves.
