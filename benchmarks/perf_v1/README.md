# Performance Baseline v1

The cost map of layered execution at `ed364ff`, before any planner cost choice
exists. Findings and interpretation are in `docs/PERFORMANCE_BASELINE_V1.md`;
this directory holds the harness and the unedited runs behind it.

```
run.sh                   join+aggregate across every representation, three
                         ranges, plus the Parquet-only scan shapes
cold.sh                  the cold series: postmaster restart + drop_caches
groupagg.sh              group aggregate at three group cardinalities
pg-restart-for-cold.sh   restart helper that preserves the XPB_S3_* environment
raw/<stamp>-<commit>[-cold|-groupagg]/   unedited psql output, one file per
                         arm/shape/state, with the command and environment
summary/<stamp>-<commit>[...].csv        one row per measured run
```

Three things about reading these numbers, all of them load-bearing:

* **wall time comes from psql, not from the modules' `total_ms`.** They differ by
  as much as 153 ms: `total_ms` covers the instrumented region and excludes
  source construction (the ZLFS zone load, a Parquet open, an HTTP connect) and
  result materialization. `wall - total` is labelled uninstrumented and is never
  called the time of a layer.
* **four states are kept apart** -- cold, first call in a fresh backend, second
  call in that backend, and warm -- because that is where the per-backend costs
  are visible. They are never averaged together.
* **no arm is timed before it matches the PostgreSQL oracle** on that shape, on
  group count, checksum and row count. `raw/20261003T172155Z-ed364ff/` is a run
  the gate refused, kept on purpose.

Usage, with the postmaster already running and carrying `XPB_S3_*`:

```
PGUSER=pguser benchmarks/perf_v1/run.sh      5420 /tmp/xpb_sock testdb
PGUSER=pguser benchmarks/perf_v1/groupagg.sh 5420 /tmp/xpb_sock testdb
             benchmarks/perf_v1/cold.sh      5420 /tmp/xpb_sock testdb   # needs root
```

`cold.sh` restarts the postmaster and drops the page cache, so it needs root and
will interrupt anything else using that server. The ZLFS arm needs a zone built
for the exact range being measured -- see the document; zone selection is exact
match, not containment.
