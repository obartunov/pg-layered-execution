#!/usr/bin/env python3
"""
Correctness harness for the XPBatch Parquet source.

  benchmarks/08-parquet-source/check.py <PGPORT> [PGHOST] [DB]

Three arms over the same logical rows:

  postgres   PostgreSQL over pq_orders, loaded from orders.csv
  pyarrow    pyarrow over orders.parquet, the same file XPBatch will read
  xpbatch    the Parquet provider, once it exists (commit 0002)

DuckDB was the task's suggested third party and is NOT available in this
container (no package, no binary). PostgreSQL + pyarrow is the substitution:
two independent implementations, one of which reads the actual Parquet file.
Recorded here rather than quietly swapped.

Each case is defined ONCE, as a name plus the SQL and the pyarrow expression
that must mean the same thing. Writing the semantics twice in two scripts is how
two arms drift apart and agree on being wrong, so they live side by side where a
reader can check them against each other.

Comparison is exact. No tolerances: every value is an integer by construction.
"""
import os
import subprocess
import sys

import pyarrow.compute as pc
import pyarrow.parquet as pq

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
PARQUET = os.path.join(DATA, "orders.parquet")

PORT = sys.argv[1] if len(sys.argv) > 1 else "5432"
HOST = sys.argv[2] if len(sys.argv) > 2 else ""
DB = sys.argv[3] if len(sys.argv) > 3 else "pqtest"
SRVUSER = os.environ.get("SRVUSER", "pguser")
PGBIN = os.environ.get("PGBIN", "/home/claude/pginstall20/bin")

# Row-group date ranges, from the generator: rg i covers
# [20260101 + i*100 .. 20260200 + i*100]. Used only to name the cases that are
# supposed to select one or several row groups; nothing here prunes.
RG_LO = [20260101 + i * 100 for i in range(8)]
RG_HI = [20260200 + i * 100 for i in range(8)]


def psql(sql, db=None):
    """Run one statement as the server user; return the last output line."""
    cmd = f"psql -p {PORT} {f'-h {HOST}' if HOST else ''} -d {db or DB} -X -qAt -c \"{sql}\""
    out = subprocess.run(
        ["su", SRVUSER, "-c", f"PATH={PGBIN}:$PATH {cmd}"],
        capture_output=True, text=True)
    lines = [l for l in (out.stdout + out.stderr).strip().splitlines() if l]
    return lines[-1] if lines else ""


def psql_rows(sql):
    cmd = f"psql -p {PORT} {f'-h {HOST}' if HOST else ''} -d {DB} -X -qAt -F'|' -c \"{sql}\""
    out = subprocess.run(
        ["su", SRVUSER, "-c", f"PATH={PGBIN}:$PATH {cmd}"],
        capture_output=True, text=True)
    if out.returncode != 0 or "ERROR" in out.stderr:
        return "ERROR: " + (out.stderr.strip().splitlines() or ["?"])[0]
    return [l for l in out.stdout.strip().splitlines() if l]


# ──────────────────────────────────────────────────────────────── load ──

def load_postgres():
    psql(f"SELECT 'x'", db="postgres")
    psql("DROP DATABASE IF EXISTS " + DB, db="postgres")
    psql("CREATE DATABASE " + DB, db="postgres")
    schema = open(os.path.join(DATA, "schema.sql")).read().replace('"', '\\"')
    for stmt in [s.strip() for s in schema.split(";") if s.strip()
                 and not s.strip().startswith("--")]:
        psql(stmt.replace("\n", " "))
    csv_path = os.path.join(DATA, "orders.csv")
    os.chmod(csv_path, 0o644)
    r = psql(f"\\copy pq_orders FROM '{csv_path}' WITH (FORMAT csv, HEADER true)")
    n = psql("SELECT count(*) FROM pq_orders")
    return n


# ──────────────────────────────────────────────────────────────── cases ──
# Each entry: (name, sql, arrow callable). The callable receives the full
# pyarrow Table and returns the same shape the SQL does: a scalar as str, or a
# list of "a|b" strings sorted the same way the SQL sorts.

def _sum(col, mask=None):
    t = TBL if mask is None else TBL.filter(mask)
    v = pc.sum(t[col])
    return "" if v.as_py() is None else str(v.as_py())


def _count(mask=None):
    return str(TBL.num_rows if mask is None else pc.sum(mask.cast("int64")).as_py() or 0)


def _groupby_sum(mask, key, val):
    t = TBL.filter(mask) if mask is not None else TBL
    g = t.group_by(key).aggregate([(val, "sum")])
    out = []
    for k, s in zip(g[key].to_pylist(), g[f"{val}_sum"].to_pylist()):
        out.append(f"{k}|{'' if s is None else s}")
    return sorted(out)


def _ge(col, v):
    return pc.greater_equal(TBL[col], v)


def _between(col, lo, hi):
    return pc.and_(pc.greater_equal(TBL[col], lo), pc.less_equal(TBL[col], hi))


CASES = [
    # name, sql, arrow
    ("count all rows",
     "SELECT count(*) FROM pq_orders",
     lambda: _count()),

    ("sum, nullable column with NULLs",
     "SELECT coalesce(sum(amount)::text,'') FROM pq_orders",
     lambda: _sum("amount")),

    ("sum, NOT NULL column",
     "SELECT sum(pad_a)::text FROM pq_orders",
     lambda: _sum("pad_a")),

    ("count non-NULL amount (NULL behaviour)",
     "SELECT count(amount) FROM pq_orders",
     lambda: str(TBL.num_rows - TBL["amount"].null_count)),

    ("filter, one row group",
     f"SELECT count(*) FROM pq_orders WHERE order_date_key BETWEEN {RG_LO[2]} AND {RG_HI[2]}",
     lambda: _count(_between("order_date_key", RG_LO[2], RG_HI[2]))),

    ("filter, several row groups",
     f"SELECT count(*) FROM pq_orders WHERE order_date_key BETWEEN {RG_LO[1]} AND {RG_HI[3]}",
     lambda: _count(_between("order_date_key", RG_LO[1], RG_HI[3]))),

    ("filter + aggregate",
     f"SELECT coalesce(sum(amount)::text,'') FROM pq_orders WHERE order_date_key >= {RG_LO[5]}",
     lambda: _sum("amount", _ge("order_date_key", RG_LO[5]))),

    ("filter selecting the all-NULL row group",
     f"SELECT count(*)||'/'||coalesce(sum(amount)::text,'NULL') FROM pq_orders "
     f"WHERE order_date_key BETWEEN {RG_LO[3]} AND {RG_HI[3]}",
     lambda: (lambda m: f"{_count(m)}/"
                        f"{'NULL' if pc.sum(TBL.filter(m)['amount']).as_py() is None else _sum('amount', m)}"
              )(_between("order_date_key", RG_LO[3], RG_HI[3]))),

    ("empty result, predicate matches nothing",
     "SELECT count(*) FROM pq_orders WHERE order_date_key > 20990101",
     lambda: _count(pc.greater(TBL["order_date_key"], 20990101))),

    ("all rows, predicate matches everything",
     "SELECT count(*) FROM pq_orders WHERE order_date_key >= 0",
     lambda: _count(_ge("order_date_key", 0))),

    ("boundary values survive, int32",
     "SELECT min(quantity)||'/'||max(quantity) FROM pq_orders",
     lambda: f"{pc.min(TBL['quantity']).as_py()}/{pc.max(TBL['quantity']).as_py()}"),

    ("boundary values survive, int64",
     "SELECT min(pad_d)||'/'||max(pad_d) FROM pq_orders",
     lambda: f"{pc.min(TBL['pad_d']).as_py()}/{pc.max(TBL['pad_d']).as_py()}"),

    ("group by, 1000 groups",
     "SELECT customer_id||'|'||coalesce(sum(amount)::text,'') FROM pq_orders "
     "GROUP BY customer_id ORDER BY customer_id",
     lambda: _groupby_sum(None, "customer_id", "amount")),

    ("group by + filter, representative query",
     f"SELECT customer_id||'|'||coalesce(sum(amount)::text,'') FROM pq_orders "
     f"WHERE order_date_key >= {RG_LO[6]} GROUP BY customer_id ORDER BY customer_id",
     lambda: _groupby_sum(_ge("order_date_key", RG_LO[6]), "customer_id", "amount")),
]


def canon(v):
    """Make the two arms comparable: a list is sorted, a scalar is a string."""
    if isinstance(v, list):
        return sorted(v)
    return str(v)


def main():
    global TBL

    if not os.path.exists(PARQUET):
        print(f"missing {PARQUET}; run gen_data.py data first", file=sys.stderr)
        return 2

    print("############ Parquet source correctness ############")
    print()
    md = pq.ParquetFile(PARQUET).metadata
    print(f"file:    {md.num_rows} rows, {md.num_row_groups} row groups, "
          f"{md.num_columns} columns, {os.path.getsize(PARQUET)} bytes")
    print("oracle:  PostgreSQL (imported) + pyarrow (same file).")
    print("         DuckDB was unavailable in this container and was NOT added")
    print("         merely to satisfy the original wording.")
    print()

    n = load_postgres()
    print(f"loaded into PostgreSQL: {n} rows")
    if n != str(md.num_rows):
        print(f"  LOAD MISMATCH: parquet {md.num_rows} vs postgres {n}")
        return 1
    print()

    TBL = pq.read_table(PARQUET)

    npass = nfail = 0
    for name, sql, arrow in CASES:
        want = psql_rows(sql) if "GROUP BY" in sql else psql(sql)
        if isinstance(want, str) and want.startswith("ERROR"):
            print(f"  BROKEN  {name} — postgres: {want}")
            nfail += 1
            continue
        try:
            got = arrow()
        except Exception as e:                      # noqa: BLE001
            print(f"  BROKEN  {name} — pyarrow raised: {e}")
            nfail += 1
            continue

        if canon(want) == canon(got):
            shown = canon(got)
            if isinstance(shown, list):
                shown = f"{len(shown)} groups, first {shown[0]}"
            print(f"  ok      {name} (= {shown})")
            npass += 1
        else:
            print(f"  DIFFER  {name}")
            print(f"            postgres {str(canon(want))[:90]}")
            print(f"            pyarrow  {str(canon(got))[:90]}")
            nfail += 1

    print()
    print(f"############ {npass} agree, {nfail} differ ############")
    print()
    print("xpbatch arm: not present until commit 0002 adds the provider.")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
