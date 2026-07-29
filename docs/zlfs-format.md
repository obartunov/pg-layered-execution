# ZLFS file format (v2)

## File structure

```
[ZlfsFileHeader]  128 bytes
[column 0 data]   nrows × 4 bytes
[column 1 data]   nrows × 4 bytes
...
[column N data]   nrows × 4 bytes
```

## Header fields

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | magic | `0x5A4C4653` ("ZLFS") |
| 4 | 4 | version | 2 |
| 8 | 4 | source_relid | OID of source relation |
| 12 | 4 | pred_lo | predicate range lower bound |
| 16 | 4 | pred_hi | predicate range upper bound |
| 20 | 4 | ncols | number of columns |
| 24 | 8 | nrows | number of rows |
| 32 | 4 | col_width | sizeof(int32) = 4 |
| 36 | 8 | build_time | TimestampTz |
| 44 | 4 | freshness | VALID=1, STALE=2 |
| 48 | 16 | col_attnos | int16[8] — column attribute numbers |
| 64 | 4 | schema_hash | FNV-1a of (attno, typid, attlen, typmod, align, byval, notnull) |
| 68 | 8 | _reserved | padding to 128 bytes |

## Schema validation

On load, the schema hash is recomputed against the current catalog.
If it differs, the zone is skipped with a WARNING.

## Write protocol

1. Write to temporary file
2. `fflush` + `pg_fsync`
3. Atomic `rename` to final path
4. Freshness updates use `fcntl F_WRLCK` + `pg_pwrite` + `pg_fsync`
