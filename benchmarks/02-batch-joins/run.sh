#!/bin/bash
#
# Experiment 2 — source -> BatchHashJoin(dim_period) -> BatchHashJoin(dim_account) -> Agg
#
#   benchmarks/02-batch-joins/run.sh [PGPORT] [PGHOST] [DBNAME]
#
# Assumes benchmarks/common/load.sh has run against the same database.
#
# Seven paths over the same rows:
#
#   vanilla            plain SQL over the heap table          (PostgreSQL executor)
#   native_columnar    plain SQL over the columnar table      (pgcolumnar custom scan)
#   xpb_heap           xp_batch pipeline, heap source
#   xpb_zlfs           xp_batch pipeline, ZLFS source
#   xpb_pgcolumnar     xp_batch pipeline, pgcolumnar fold source
#   xpb_parquet        xp_batch pipeline, Parquet source via the provider registry
#   duckdb_parquet     DuckDB over the SAME Parquet file      (third-party engine)
#
# The two Parquet arms were added without changing the logical query, the slice,
# the joins, the grouping or the checksum. They are conditional on the exported
# Parquet file being present (export-parquet.py) and on the xpb_parquet module
# being installed; absence skips them VISIBLY, never silently.
#
# xpb_parquet reaches the pipeline through the generic mode string
# ext:<provider>:<uri>, resolved in the source registry. No branch in the
# pipeline names Parquet.
#
# native_columnar is not optional padding: without it the comparison only shows
# that a batch pipeline beats the row executor, not whether reading through the
# fold API beats the columnar engine's own scan.
#
# The correctness gate runs first. Timings are taken only once it passes, and
# only from warm runs.
set -euo pipefail

PORT="${1:-5432}"
PGHOST_ARG="${2:-}"
DB="${3:-testdb}"
LO=25
HI=36

PSQL=(psql -p "$PORT" -d "$DB" -v ON_ERROR_STOP=1 -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

HAVE_COL=$("${PSQL[@]}" -c "SELECT to_regclass('reg_buh_col') IS NOT NULL")
[ "$HAVE_COL" = "t" ] || echo "!! reg_buh_col absent: columnar paths skipped"

# ── Parquet arms ──
# Two independent preconditions, reported separately: the file has to exist, and
# the optional module has to load. A LOAD failure must not be read as "no file".
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PQ_DIR="${PQ_DIR:-$HERE/data}"
PQ_FILE="$PQ_DIR/reg_buh.parquet"
HAVE_PQ=f
case "$PQ_FILE" in
    # The path travels through a space-separated mode list and an SQL literal.
    # Refuse rather than fail obscurely three steps later.
    *[[:space:]]*|*"'"*) echo "!! PQ_DIR must not contain whitespace or quotes"; exit 2 ;;
esac
if [ ! -f "$PQ_FILE" ]; then
    echo "!! $PQ_FILE absent: Parquet arms skipped (run export-parquet.py)"
elif ! "${PSQL[@]}" -c "LOAD 'xpb_parquet'" >/dev/null 2>&1; then
    echo "!! xpb_parquet module will not load: Parquet arms skipped"
else
    HAVE_PQ=t
fi

HAVE_DUCK=f
if [ "$HAVE_PQ" = "t" ] && python3 -c "import duckdb" 2>/dev/null; then
    HAVE_DUCK=t
else
    [ "$HAVE_PQ" = "t" ] && echo "!! duckdb not importable: DuckDB arm skipped"
fi

# total_ms is a timing column and must stay out of the checksum, or no two runs
# of the same path can ever compare equal.
CK="md5(string_agg(year||','||account_group||','||company_key||','||total_amt,
    '|' ORDER BY year, account_group, company_key))"

SQL_BODY="SELECT d.year, a.account_group, r.company_key,
                 sum(r.amount_dt)::bigint AS total_amt
          FROM %s r
          JOIN dim_period  d ON d.period_key  = r.period_key
          JOIN dim_account a ON a.account_key = r.account_key
          WHERE r.period_key BETWEEN $LO AND $HI
          GROUP BY d.year, a.account_group, r.company_key"

echo "=== setup: ZLFS zone [$LO..$HI] ==="
"${PSQL[@]}" -c "CREATE EXTENSION IF NOT EXISTS xp_batch" >/dev/null
"${PSQL[@]}" -c "SELECT zlfs_build_zone('reg_buh', '1,2,3,6', $LO, $HI)" >/dev/null

echo "=== correctness gate ==="
{
cat <<SQL
SET max_parallel_workers_per_gather = 0;
SET jit = off;
CREATE TEMP TABLE ck(path text, ck text, groups bigint, ord int);

INSERT INTO ck SELECT 'vanilla', $CK, count(*), 1
FROM ($(printf "$SQL_BODY" reg_buh)) s;

INSERT INTO ck SELECT 'xpb_heap', $CK, count(*), 3
FROM xpb_batch_join2_groupby($LO, $HI, 'heap');

INSERT INTO ck SELECT 'xpb_zlfs', $CK, count(*), 4
FROM xpb_batch_join2_groupby($LO, $HI, 'zlfs');

-- repeat in the same backend: state left behind shows up here, not in the data
INSERT INTO ck SELECT 'xpb_zlfs_again', $CK, count(*), 6
FROM xpb_batch_join2_groupby($LO, $HI, 'zlfs');
SQL
if [ "$HAVE_COL" = "t" ]; then
cat <<SQL
INSERT INTO ck SELECT 'native_columnar', $CK, count(*), 2
FROM ($(printf "$SQL_BODY" reg_buh_col)) s;

INSERT INTO ck SELECT 'xpb_pgcolumnar', $CK, count(*), 5
FROM xpb_batch_join2_groupby($LO, $HI, 'pgcolumnar');

INSERT INTO ck SELECT 'xpb_pgcolumnar_again', $CK, count(*), 7
FROM xpb_batch_join2_groupby($LO, $HI, 'pgcolumnar');
SQL
fi
if [ "$HAVE_PQ" = "t" ]; then
cat <<SQL
LOAD 'xpb_parquet';

INSERT INTO ck SELECT 'xpb_parquet', $CK, count(*), 8
FROM xpb_batch_join2_groupby($LO, $HI, 'ext:parquet:$PQ_FILE');

-- again in the same backend, for the same reason as the zlfs repeat: a reader
-- that leaks row-group state across constructions shows up here
INSERT INTO ck SELECT 'xpb_parquet_again', $CK, count(*), 9
FROM xpb_batch_join2_groupby($LO, $HI, 'ext:parquet:$PQ_FILE');
SQL
fi
cat <<'SQL'
SELECT rpad(path, 22) || ck || '  groups=' || groups FROM ck ORDER BY ord;
DO $$
DECLARE n int;
BEGIN
    SELECT count(DISTINCT ck) INTO n FROM ck;
    IF n <> 1 THEN RAISE EXCEPTION 'CHECKSUM MISMATCH across paths'; END IF;
    SELECT count(DISTINCT groups) INTO n FROM ck;
    IF n <> 1 THEN RAISE EXCEPTION 'GROUP COUNT MISMATCH across paths'; END IF;
    RAISE NOTICE 'gate PASS';
END $$;
SQL
} | "${PSQL[@]}"

# DuckDB is a separate process, so it cannot join the temp table above. Its
# checksum is compared against the one PostgreSQL computes over the heap table --
# the arm that defines the right answer -- and a mismatch is fatal here exactly
# as it is inside the gate.
if [ "$HAVE_DUCK" = "t" ]; then
    echo "=== third-party gate: DuckDB over the same Parquet file ==="
    WANT=$("${PSQL[@]}" <<SQL
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SELECT $CK || '  groups=' || count(*) FROM ($(printf "$SQL_BODY" reg_buh)) s;
SQL
)
    GOT=$(python3 "$HERE/duckdb-arm.py" check "$PQ_DIR")
    GOT="${GOT%|*}  groups=${GOT#*|}"
    echo "postgres/heap          $WANT"
    echo "duckdb/parquet         $GOT"
    [ "$WANT" = "$GOT" ] || { echo "!! DUCKDB CHECKSUM MISMATCH"; exit 1; }
    echo "NOTICE:  third-party gate PASS"
fi

echo "=== EXPLAIN: each SQL path takes the plan it is named for ==="
# same GUCs as the timed runs, or the plan shown is not the plan measured
for tbl in reg_buh reg_buh_col; do
    [ "$tbl" = "reg_buh_col" ] && [ "$HAVE_COL" != "t" ] && continue
    echo "-- $tbl"
    "${PSQL[@]}" <<SQL | grep -E "Scan|Aggregate|Join" | head -6
SET max_parallel_workers_per_gather = 0;
SET jit = off;
EXPLAIN (COSTS OFF) $(printf "$SQL_BODY" $tbl);
SQL
done

echo "=== 5 warm runs per xp_batch path ==="
MODES="heap zlfs"
[ "$HAVE_COL" = "t" ] && MODES="$MODES pgcolumnar"
[ "$HAVE_PQ" = "t" ] && MODES="$MODES ext:parquet:$PQ_FILE"
for mode in $MODES; do
    # The provider lives in an optional module, so the ext arm has to load it;
    # the other arms must not, or a missing module would break them too.
    LOADPQ=""
    case "$mode" in ext:*) LOADPQ="LOAD 'xpb_parquet';" ;; esac
    for i in 1 2 3 4 5; do
        "${PSQL[@]}" -c "SET max_parallel_workers_per_gather=0; SET jit=off; $LOADPQ
                         SELECT count(*) FROM xpb_batch_join2_groupby($LO,$HI,'$mode')" 2>&1 >/dev/null \
        | sed -n 's/^NOTICE:  batch_join2_groupby //p'
    done
done

if [ "$HAVE_DUCK" = "t" ]; then
    echo "=== 5 warm runs, DuckDB over the same Parquet file (threads=1) ==="
    python3 "$HERE/duckdb-arm.py" time "$PQ_DIR" 5 | sed 's/^/duckdb_parquet  total=/;s/$/ ms/'
fi

echo "=== 5 warm runs per SQL path ==="
for tbl in reg_buh reg_buh_col; do
    [ "$tbl" = "reg_buh_col" ] && [ "$HAVE_COL" != "t" ] && continue
    echo "-- $tbl"
    for i in 1 2 3 4 5; do
        "${PSQL[@]}" <<SQL | grep -i '^time'
SET max_parallel_workers_per_gather = 0;
SET jit = off;
\\timing on
SELECT count(*) FROM ($(printf "$SQL_BODY" $tbl)) s;
SQL
    done
done
