#!/usr/bin/env python3
"""
Export the benchmark-02 tables to Parquet, for the Parquet/xp_batch and
DuckDB/Parquet arms.

  export-parquet.py [PORT] [PGHOST] [DB] [OUTDIR]

This is not a new dataset. The rows come out of the SAME reg_buh that every
other arm of benchmark 02 reads, in its physical order and with no ORDER BY, so
the Parquet file is a re-encoding of exactly what the heap scan sees. Anything
else would make the comparison a comparison of datasets.

ROW GROUP SIZE
--------------
50 000 rows, which is below XPCB_BATCH_CAP (65 536). That is a hard constraint,
not a tuning choice: the Parquet provider emits one batch per row group and
refuses a row group larger than the batch capacity rather than truncating it
(xpb_parquet_module.c). 10M rows therefore give 200 row groups.

reg_buh has 83 334 rows per period_key, generated in period order, so each row
group spans one or two period keys and row-group pruning has real work to do on
the [25..36] slice -- while the two boundary row groups straddle it, which is
why the predicate still has to be applied per row above the source.

TYPES
-----
int32 for every column, matching `int` in benchmarks/common/schema.sql. The
batch contract carries int4 and int8 and the pipeline reads these four columns as
int4; writing int64 here would change what is being measured.
"""
import os
import subprocess
import sys

import pyarrow as pa
import pyarrow.csv as pacsv
import pyarrow.parquet as pq

FACT_COLS = ["period_key", "company_key", "account_key", "debit_key",
             "credit_key", "amount_dt", "amount_kt", "payload"]
DIMS = {
    "dim_period":  ["period_key", "year", "month"],
    "dim_account": ["account_key", "account_group"],
}
RG_ROWS = 50_000


def copy_out(psql, table, cols):
    """COPY the table out in physical order and parse it as int32 columns."""
    sql = f"COPY (SELECT {', '.join(cols)} FROM {table}) TO STDOUT WITH (FORMAT csv)"
    proc = subprocess.Popen(psql + ["-c", sql], stdout=subprocess.PIPE)
    schema = pa.schema([(c, pa.int32()) for c in cols])
    tbl = pacsv.read_csv(
        proc.stdout,
        read_options=pacsv.ReadOptions(column_names=cols),
        convert_options=pacsv.ConvertOptions(column_types=schema),
    )
    if proc.wait() != 0:
        sys.exit(f"COPY of {table} failed")
    return tbl


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "5432"
    host = sys.argv[2] if len(sys.argv) > 2 else ""
    db = sys.argv[3] if len(sys.argv) > 3 else "testdb"
    outdir = sys.argv[4] if len(sys.argv) > 4 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "data")

    psql = ["psql", "-p", port, "-d", db, "-X", "-qAt", "-v", "ON_ERROR_STOP=1"]
    if host:
        psql += ["-h", host]

    os.makedirs(outdir, exist_ok=True)

    tbl = copy_out(psql, "reg_buh", FACT_COLS)
    path = os.path.join(outdir, "reg_buh.parquet")
    # Statistics ON: row-group pruning may use declared metadata only, so the
    # metadata has to be written. Snappy rather than uncompressed because a
    # Parquet file in production is compressed, and the provider's attributed
    # bytes are read from the footer either way.
    pq.write_table(tbl, path, row_group_size=RG_ROWS,
                   compression="snappy", write_statistics=True, version="2.6")

    for name, cols in DIMS.items():
        d = copy_out(psql, name, cols)
        pq.write_table(d, os.path.join(outdir, f"{name}.parquet"),
                       compression="snappy", write_statistics=True, version="2.6")
        print(f"{name}.parquet: rows={d.num_rows}")

    md = pq.ParquetFile(path).metadata
    print(f"reg_buh.parquet: rows={md.num_rows} row_groups={md.num_row_groups} "
          f"cols={md.num_columns} bytes={os.path.getsize(path)}")

    # Which row groups a [25..36] predicate can exclude from declared bounds
    # alone, and which straddle the boundary. Printed because it is the number
    # the Parquet arm's pruning counters have to agree with.
    lo, hi = 25, 36
    inside = straddle = outside = 0
    for i in range(md.num_row_groups):
        st = md.row_group(i).column(0).statistics
        if st.max < lo or st.min > hi:
            outside += 1
        elif st.min >= lo and st.max <= hi:
            inside += 1
        else:
            straddle += 1
    print(f"period_key [{lo}..{hi}]: row groups fully inside={inside} "
          f"straddling={straddle} excludable={outside}")


if __name__ == "__main__":
    main()
