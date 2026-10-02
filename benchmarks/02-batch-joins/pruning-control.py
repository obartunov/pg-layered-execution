#!/usr/bin/env python3
"""
Control for the pruning claim: the same rows in a different physical order.

  pruning-control.py <PORT> <PGHOST> <DB> [DATADIR]

reg_buh is generated in period_key order, 83 334 rows per key, so a [25..36]
predicate excludes 179 of 200 row groups from declared metadata alone. That is a
property of how the data was written, not of the Parquet source, and the
difference between the two is the whole question when a number like "109x fewer
bytes" gets attributed to a layer.

This writes reg_buh_shuffled.parquet -- the SAME rows, the same row-group size,
the same statistics, in a fixed-seed random order -- and reports what the
provider's own counters say about both files. The shuffled file must also produce
the same GROUP BY answer, which is checked here: shuffling rows cannot change an
aggregate, and if it did, something other than pruning is wrong.

Not part of run.sh. It answers a question about the dataset, not about an arm.
"""
import os
import subprocess
import sys

import numpy as np
import pyarrow.parquet as pq

SEED = 20261002
COLS = "period_key,company_key,account_key,amount_dt"


def psql(args, sql):
    out = subprocess.run(args + ["-c", sql], capture_output=True, text=True)
    return (out.stdout + out.stderr).strip().splitlines()[-1] if (out.stdout or out.stderr) else ""


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "5432"
    host = sys.argv[2] if len(sys.argv) > 2 else ""
    db = sys.argv[3] if len(sys.argv) > 3 else "testdb"
    datadir = sys.argv[4] if len(sys.argv) > 4 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "data")

    args = ["psql", "-p", port, "-d", db, "-X", "-qAt", "-v", "ON_ERROR_STOP=1"]
    if host:
        args += ["-h", host]

    src = os.path.join(datadir, "reg_buh.parquet")
    dst = os.path.join(datadir, "reg_buh_shuffled.parquet")
    if not os.path.exists(src):
        sys.exit(f"{src} absent — run export-parquet.py first")

    if not os.path.exists(dst):
        tbl = pq.read_table(src)
        md = pq.ParquetFile(src).metadata
        rg_rows = md.row_group(0).num_rows
        perm = np.random.default_rng(SEED).permutation(tbl.num_rows)
        pq.write_table(tbl.take(perm), dst, row_group_size=rg_rows,
                       compression="snappy", write_statistics=True, version="2.6")
        print(f"wrote {dst} ({os.path.getsize(dst)} bytes, seed {SEED})")
    else:
        print(f"reusing {dst}")

    probe = ("SELECT row_groups_total||' '||row_groups_read||' '||row_groups_skipped"
             "||' '||rows||' '||data_bytes FROM xpq_scan('%s','" + COLS + "',25,36)")

    print(f"\n{'file':14s} {'rg_total':>8} {'rg_read':>8} {'rg_skipped':>10} {'rows':>9} {'data_bytes':>11}")
    for label, path in (("clustered", src), ("shuffled", dst)):
        line = psql(args, "LOAD 'xpb_parquet'; " + probe % path)
        parts = line.split()
        if len(parts) != 5:
            print(f"{label:14s} probe failed: {line}")
            continue
        t, r, s, rows, b = parts
        print(f"{label:14s} {t:>8} {r:>8} {s:>10} {rows:>9} {b:>11}")

    # Same answer from both files, or the control is measuring two datasets.
    ck = ("SELECT md5(string_agg(year||','||account_group||','||company_key||','||total_amt,"
          "'|' ORDER BY year, account_group, company_key))||'|'||count(*) "
          "FROM xpb_batch_join2_groupby(25,36,'ext:parquet:%s')")
    print()
    for label, path in (("clustered", src), ("shuffled", dst)):
        print(f"{label:14s} {psql(args, 'LOAD ' + chr(39) + 'xpb_parquet' + chr(39) + '; SET client_min_messages=warning; ' + ck % path)}")


if __name__ == "__main__":
    main()
