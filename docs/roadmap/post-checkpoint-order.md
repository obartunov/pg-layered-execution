# Order of work after the reviewed checkpoint

Reviewed checkpoint: **`4d58ffdc77cbe698a08b19d449bdfb503a425baf`**, 47 commits,
accepted by both review gates (@Teodor mechanical/numerical, @yoda
architectural). Captured as an incremental git bundle over `origin/main`
(`3eb21db`); `origin` itself is untouched at the time of writing.

```
Dimension Hash Growth v1        DONE, ccb897c -- housekeeping, removed a confound
        |
Fixed Hash Silent-Drop          correctness gate, short
Reachability
        |
05-F Heap Block-Range Pruning   research milestone
        |
Skew
        |
10M scale
```

Revised by @obe after the Dimension Hash Growth review: the reachability
question now precedes 05-F, because a potential silent wrong answer outranks
the next benchmark.

## Why this order

**Dimension Hash Growth v1 came first, and it was not a research milestone.**
`reg2_card2` reached 147 456 groups only by holding both dimension hashes at
their 3/4 load limit — 192 of 256 slots and 768 of 1024. Skew attacks load
factor. Run skew against saturated dimension tables and a regression cannot be
attributed: group hash and dimension hash degrade together. The fix was the same
single-variable change already made for the group hash, applied to the
structure that had become binding. Same policy, same hash, same probing, no
spill. Done: both tables now grow at load 0.5 by doubling, measured to 4x the
old limit with join cost showing no resolved change.

**Fixed Hash Silent-Drop Reachability comes next, and it is a correctness gate,
not a benchmark.** `docs/roadmap/fixed-hash-silent-drop.md` has the detail. The
single question it must answer:

> can any benchmark or shipped path in this repository fill
> `xpb_projection.c`'s `AGG_CAP` or `xpb_columnar_pipeline.c`'s `WHASH_CAP` to
> capacity?

Reproducer and reachability first, not a fix. If reachable, fix and revisit the
affected published benchmark results — the count of shipped `reg_buh` shapes has
to happen before the fix, because afterwards the old behaviour is unobservable.
If unreachable on current shapes, document the boundary so the next person to
relax a type gate or grow a dataset sees it. `xpb_groupagg2.c` is carried in the
same task as a third, weaker case: no pre-insert guard, a post-scan check at
0.95, and a capacity that comes from a planner estimate rather than a constant.

**05-F Heap Block-Range Pruning is the research milestone, and it comes before
skew.** The series produced two pruning granularities and left the middle one
missing:

```
rowgroup pruning     pgColumnar            measured, 05-A
block / page-range   MISSING
tuple rejection      projected-early       measured, 05-D
```

05-D concluded that the residual gap to pgColumnar is *scan granularity*, not
decode — and then every following milestone went downstream instead. The
dataset is already clustered on `period` and pgColumnar's rowgroup maps are
there to compare against, so the experiment is cheap. The question it answers
is the strongest one the series has raised:

> how much of the columnar advantage is the columnar *representation*, and how
> much is simply not visiting physical regions that cannot match?

05-F should also settle 05-D's unmeasured premise: predicate position (attnum
1 / 5 / 9). Block pruning and tuple rejection depend on it differently, and
05-D's 1.4–1.8x is its best case, with the predicate at physical attnum 1.

**Skew after 05-F**, because every probe figure in the series rests on a
uniform distribution, and probe stability is exactly what a hot key attacks.
Two hash populations will be in scope by then, both with bounded load, which is
what makes the result attributable.

**10M scale last.** It needed the dimension caps raised first, which step 1
did.

## Carried separately, not folded into any of the above

`docs/roadmap/groupagg2-int64-overflow.md` — unchecked int64 accumulation in
`XpGroupAgg2` feeding `int8_numeric()`, a possible silent wrong answer on a
different execution path. Reachability unproven, so the task starts with a
reproducer rather than a fix. It must not be merged into a benchmark milestone:
it is a correctness question and deserves its own answer.

## Standing methodology, learned the hard way in the checkpoint

* Preregister a policy before measuring it, and do not tune it from the first
  numbers.
* Timings are comparable only within a session. This host has drifted 1.6x
  between days and measurably within a day on code that did not change.
* A number in a write-up must be re-derivable from an artefact in `raw/`.
  `benchmarks/05-1c-like-v2/check-summaries.py` is the mechanical check.
* Split a metric before publishing it if it could answer two questions —
  current-capacity versus lifetime, pooled versus final-table. Both ambiguities
  had to be fixed after the fact once.
* Per-group, not grand-total, comparison against PostgreSQL. A lost or
  duplicated group keeps the row count right.
