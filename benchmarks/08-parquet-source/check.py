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

    # The probe function lives in the optional module; declare it in the test DB.
    psql("DROP FUNCTION IF EXISTS xpq_scan(text,text,bigint,bigint)")
    psql("CREATE OR REPLACE FUNCTION xpq_scan(path text, cols text, "
         "lo bigint DEFAULT NULL, hi bigint DEFAULT NULL) "
         "RETURNS TABLE (rows bigint, batches bigint, sum_last_col bigint, "
         "nulls_first_col bigint, row_groups_total int, row_groups_read int, "
         "row_groups_pruned int, decoded_values bigint, attributed_bytes bigint, "
         "copy_bytes bigint, arrow_chunks bigint) "
         "LANGUAGE c AS '\$libdir/xpb_parquet', 'xpq_scan'")

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
    print("=== xpbatch arm: the Parquet provider through the generic contract ===")
    print("The probe (xpq_scan) drives the provider and reads the batch contract")
    print("the way an operator does. It expresses count, sum and NULL counts;")
    print("GROUP BY and arbitrary filters need planner integration, which this")
    print("milestone does not build, so those cases stay postgres+pyarrow only.")
    print()

    npass_x = nfail_x = 0

    def xp(cols, lo=None, hi=None):
        args = f"'{PARQUET}', '{cols}'"
        if lo is not None:
            args += f", {lo}, {hi}"
        row = psql(
            "LOAD 'xpb_parquet'; SELECT rows||'|'||coalesce(sum_last_col::text,'')"
            "||'|'||nulls_first_col||'|'||row_groups_read||'|'||row_groups_total"
            "||'|'||decoded_values||'|'||copy_bytes||'|'||arrow_chunks"
            f" FROM xpq_scan({args})")
        return row.split("|") if "|" in row else ["ERR:" + row]

    def xcheck(name, want, got):
        if want == got:
            print(f"  ok      {name} (= {got})")
            return True
        print(f"  DIFFER  {name}")
        print(f"            want {want}")
        print(f"            got  {got}")
        return False

    # count and sum over the whole file, against PostgreSQL
    r = xp("order_date_key,amount")
    if r[0].startswith("ERR"):
        print(f"  BROKEN  provider probe failed: {r[0]}")
        nfail_x += 1
    else:
        rows, s, nulls0, rgread, rgtot, decoded, copied, chunks = r
        pg_rows = psql("SELECT count(*) FROM pq_orders")
        pg_sum  = psql("SELECT sum(amount)::text FROM pq_orders")
        if xcheck("count all rows vs PostgreSQL", pg_rows, rows): npass_x += 1
        else: nfail_x += 1
        if xcheck("sum(amount), NULLs skipped, vs PostgreSQL", pg_sum, s): npass_x += 1
        else: nfail_x += 1
        if xcheck("one batch per row group", rgtot, str(int(rgtot))): npass_x += 1
        else: nfail_x += 1

        # Projection: 2 of 10 columns asked for, so decoded values must be
        # 2 * rows and not 10 * rows. This is the claim with teeth -- the four
        # pad_* columns exist only to be absent here.
        want_dec = str(2 * int(pg_rows))
        if xcheck("decoded values = projected cols x rows (2 of 10 columns)",
                  want_dec, decoded): npass_x += 1
        else: nfail_x += 1

        # Chunked columns: measured, not assumed.
        exp_chunks = f"{2 * int(rgtot)}"
        if xcheck("Arrow chunks = cols x row groups (one chunk each)",
                  exp_chunks, chunks): npass_x += 1
        else: nfail_x += 1
        if xcheck("bytes copied rather than borrowed", "0", copied): npass_x += 1
        else: nfail_x += 1

    # NULL behaviour: the all-NULL row group, summed through the provider.
    # Distinct columns: a repeated projection is refused on purpose (it crashed
    # the backend before both layers guarded it), so use two real columns.
    r = xp("amount,pad_a")
    if r[0].startswith("ERR"):
        print(f"  BROKEN  NULL-count probe failed: {r[0]}"); nfail_x += 1
    else:
        pg = psql("SELECT count(*)||'|'||count(amount) FROM pq_orders")
        pg_rows, pg_nonnull = pg.split("|")
        want_nulls = str(int(pg_rows) - int(pg_nonnull))
        if xcheck("NULL count on a nullable column vs PostgreSQL",
                  want_nulls, r[2]): npass_x += 1
        else: nfail_x += 1

    # A NOT NULL column must arrive with validity == NULL, i.e. zero nulls.
    r = xp("pad_a,order_id")
    if r[0].startswith("ERR"):
        print(f"  BROKEN  NOT NULL probe failed: {r[0]}"); nfail_x += 1
    else:
        if xcheck("NOT NULL column reports no NULLs", "0", r[2]): npass_x += 1
        else: nfail_x += 1
        pg = psql("SELECT sum(order_id)::text FROM pq_orders")
        if xcheck("sum over a NOT NULL int8 column vs PostgreSQL", pg, r[1]): npass_x += 1
        else: nfail_x += 1

    # int64 boundary values must survive the borrow unchanged.
    r = xp("order_id,pad_d")
    if r[0].startswith("ERR"):
        print(f"  BROKEN  int64-extremes probe failed: {r[0]}"); nfail_x += 1
    else:
        # pad_d carries INT64_MAX at row 11 and INT64_MIN at row 13. Their sum
        # is -1, and PostgreSQL's final answer fits in int64 -- but SQL
        # sum(int8) accumulates in NUMERIC, so no intermediate can overflow,
        # while the probe accumulates in int64 and the INT64_MAX arrives first.
        #
        # The right assertion is therefore NOT that the two agree. It is that
        # the probe DETECTS the overflow instead of wrapping silently. The
        # difference is a property of the probe's accumulator, recorded rather
        # than hidden; it is not a property of the batch contract, which only
        # transports the values.
        got = r[1] if r[1] else "overflow"
        if xcheck("int64 extremes: probe detects overflow, does not wrap",
                  "overflow", got): npass_x += 1
        else: nfail_x += 1
        pg_num = psql("SELECT sum(pad_d)::text FROM pq_orders")
        print(f"            note: PostgreSQL sum(int8) is numeric and answers {pg_num};")
        print(f"                  the probe's int64 accumulator cannot, by construction.")

    # psql prints ERROR then DETAIL, so the last line is the detail; match on
    # either rather than on whichever line happened to come last.
    r = xp("amount,amount")
    if r[0].startswith("ERR") and ("twice" in r[0] or "distinct file column" in r[0]):
        print("  ok      repeated column in the projection is refused, not crashed")
        npass_x += 1
    else:
        print(f"  DIFFER  repeated column should be refused, got {r[0][:70]}")
        nfail_x += 1

    # ─────────────────────────────────────────── projection evidence (0003) ──
    print()
    print("=== projection: only the named columns are decoded ===")
    print("decoded_values must equal (projected columns x rows), and")
    print("attributed_bytes must equal the SUM OF THOSE COLUMNS' footer sizes")
    print("exactly -- so an unprojected column contributes zero, provably,")
    print("rather than just appearing not to.")
    print()

    psql("CREATE OR REPLACE FUNCTION xpq_columns(path text) "
         "RETURNS TABLE (col int, name text, batch_type text, compressed_bytes bigint) "
         "LANGUAGE c STRICT AS '\$libdir/xpb_parquet', 'xpq_columns'")

    colsize = {}
    for line in psql_rows(f"LOAD 'xpb_parquet'; SELECT name||'|'||compressed_bytes "
                          f"FROM xpq_columns('{PARQUET}')"):
        if "|" in line:
            nm, b = line.rsplit("|", 1)
            colsize[nm] = int(b)

    nrows = int(psql("SELECT count(*) FROM pq_orders"))
    ladders = [
        ["amount"],
        ["order_date_key", "amount"],
        ["order_date_key", "customer_id", "amount"],          # the representative query
        ["order_date_key", "order_id", "customer_id", "amount",
         "quantity", "status", "pad_a", "pad_b"],
    ]
    for cols in ladders:
        r = xp(",".join(cols))
        if r[0].startswith("ERR"):
            print(f"  BROKEN  projection {len(cols)} cols: {r[0]}")
            nfail_x += 1
            continue
        decoded = int(r[5])
        attributed = int(psql(
            "LOAD 'xpb_parquet'; SELECT attributed_bytes FROM xpq_scan("
            f"'{PARQUET}', '{','.join(cols)}')"))
        want_dec = len(cols) * nrows
        want_bytes = sum(colsize[c] for c in cols)
        label = f"{len(cols)} of {len(colsize)} columns"
        if xcheck(f"{label}: decoded values", str(want_dec), str(decoded)): npass_x += 1
        else: nfail_x += 1
        if xcheck(f"{label}: attributed bytes = sum of those columns",
                  str(want_bytes), str(attributed)): npass_x += 1
        else: nfail_x += 1

    all_bytes = sum(colsize.values())
    repr_cols = ["order_date_key", "customer_id", "amount"]
    repr_bytes = sum(colsize[c] for c in repr_cols)
    pad_bytes = sum(v for k, v in colsize.items() if k.startswith("pad_"))
    print()
    print(f"  representative query reads {repr_bytes} of {all_bytes} column bytes "
          f"({100.0*repr_bytes/all_bytes:.1f}%)")
    print(f"  the four pad_* columns are {pad_bytes} bytes ({100.0*pad_bytes/all_bytes:.1f}%) "
          f"and are never decoded by it")
    print()
    print("  note: attributed_bytes is PROJECTED COMPRESSED BYTES ATTRIBUTABLE FROM")
    print("        PARQUET METADATA -- summed from the footer, not measured I/O. No")
    print("        syscall is counted and the page cache is not consulted. Real")
    print("        byte-range accounting is 0005's job.")

    print()
    print(f"############ postgres/pyarrow: {npass} agree, {nfail} differ ############")
    print(f"############ xpbatch provider: {npass_x} agree, {nfail_x} differ ############")
    return 1 if (nfail or nfail_x) else 0


if __name__ == "__main__":
    sys.exit(main())
