# Experiment methodology

## Cache protocol

- Same backend (single psql session per path group)
- OS page cache retained between runs
- ZLFS zone file mmap'd in process, retained after build
- shared_buffers 128 MB (table 575 MB, cannot fit entirely)

## Measurement

- 1 warmup run discarded
- 5 measured runs
- Median reported
- Phase timing via `instr_time` (per-batch, not per-row)

## Correctness

- MD5 checksum of sorted result text representation
- Compared against vanilla PostgreSQL on identical data
- Deterministic data generator (seed=42)

## What "ZLFS kernel" measures

The ZLFS scan timer covers the full fused loop: column read + hash
probe + SUM transition. It is a fused scan+aggregate kernel, not a
pure scan.
