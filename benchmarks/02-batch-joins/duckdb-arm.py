#!/usr/bin/env python3
"""
DuckDB/Parquet arm of benchmark 02.

  duckdb-arm.py check <datadir>        -> "<md5>|<groups>"
  duckdb-arm.py time  <datadir> <n>    -> n lines, one wall-clock ms each

Same logical query as every other arm: the same 12-period slice, the same two
dimension joins, the same GROUP BY. It reads the SAME reg_buh.parquet that the
Parquet/xp_batch arm reads -- not a DuckDB-native table, not a re-export.

THREADS = 1 is not a handicap, it is the comparison. Every other arm runs with
max_parallel_workers_per_gather = 0, so a multi-threaded DuckDB would be
measuring parallelism against four single-threaded pipelines.

The timed query is `count(*)` over the grouped result, exactly as run.sh times
the SQL arms, so the md5/string_agg of the correctness gate is not inside any
timing. Wall clock is measured in this process and therefore includes DuckDB's
own query setup but not interpreter start.
"""
import sys
import time

import duckdb

BODY = """
    SELECT d.year AS year, a.account_group AS account_group,
           r.company_key AS company_key,
           CAST(sum(r.amount_dt) AS BIGINT) AS total_amt
    FROM read_parquet('{d}/reg_buh.parquet') r
    JOIN read_parquet('{d}/dim_period.parquet') d  ON d.period_key  = r.period_key
    JOIN read_parquet('{d}/dim_account.parquet') a ON a.account_key = r.account_key
    WHERE r.period_key BETWEEN 25 AND 36
    GROUP BY 1, 2, 3
"""

# Same checksum definition as run.sh: '|'-joined "year,account_group,company_key,
# total_amt" rows in that order, md5 of the whole string. DuckDB needs the casts
# spelled out where PostgreSQL's || coerces.
CHECK = """
SELECT md5(string_agg(CAST(year AS VARCHAR) || ',' || CAST(account_group AS VARCHAR)
           || ',' || CAST(company_key AS VARCHAR) || ',' || CAST(total_amt AS VARCHAR),
           '|' ORDER BY year, account_group, company_key)), count(*)
FROM ({body}) g
"""


def connect():
    con = duckdb.connect()
    con.execute("SET threads TO 1")
    return con


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    what, datadir = sys.argv[1], sys.argv[2].rstrip("/")
    body = BODY.format(d=datadir)
    con = connect()

    if what == "check":
        ck, groups = con.execute(CHECK.format(body=body)).fetchone()
        print(f"{ck}|{groups}")
        return

    if what == "time":
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 5
        sql = f"SELECT count(*) FROM ({body}) s"
        con.execute(sql).fetchone()            # warm the page cache, discard
        for _ in range(n):
            t0 = time.perf_counter()
            con.execute(sql).fetchone()
            print(f"{(time.perf_counter() - t0) * 1000:.1f}")
        return

    sys.exit(f"unknown command: {what}")


if __name__ == "__main__":
    main()
