# Contributing

This is a research prototype. Contributions should maintain the
experimental methodology: correctness first, then measurement.

## Before submitting

1. All paths must produce the same checksum as vanilla PostgreSQL
2. Raw measurement data (5+ warm runs) must accompany any new numbers
3. Scope should be stated explicitly — what the change proves and what it does not
4. Build must pass with `--enable-cassert`

## Code style

Follow PostgreSQL coding conventions. Use tabs for indentation in C code.

## Experiments

New experiments should follow the structure in `benchmarks/`:
- `setup.sql` — schema and data
- `query.sql` — the measured query
- `run.sh` — automated measurement
- `expected-checksum.txt` — correctness anchor
- `results.csv` — generated from raw data, not hand-edited
