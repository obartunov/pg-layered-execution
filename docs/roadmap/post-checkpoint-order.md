# Order of work after the reviewed checkpoint

Reviewed checkpoint: **`4d58ffdc77cbe698a08b19d449bdfb503a425baf`**, 47 commits,
accepted by both review gates (@Teodor mechanical/numerical, @yoda
architectural). Captured as an incremental git bundle over `origin/main`
(`3eb21db`); `origin` itself is untouched at the time of writing.

```
Dimension Hash Growth v1        housekeeping, removes a confound
        |
05-F Heap Block-Range Pruning   research milestone
        |
Skew
        |
10M scale
```

## Why this order

**Dimension Hash Growth v1 comes first, and it is not a research milestone.**
`reg2_card2` reaches 147 456 groups only by holding both dimension hashes at
their 3/4 load limit — 192 of 256 slots and 768 of 1024. Skew attacks load
factor. Run skew against saturated dimension tables and a regression cannot be
attributed: group hash and dimension hash degrade together. The fix is the same
single-variable change already made for the group hash, applied to the
structure that is now binding. Same policy, same hash, same probing, no spill.

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

**Skew after that**, because every probe figure in the series rests on a
uniform distribution, and probe stability is exactly what a hot key attacks.
Two hash populations will be in scope by then, both with bounded load, which is
what makes the result attributable.

**10M scale last.** It needs the dimension caps raised first, which is step 1
anyway.

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
