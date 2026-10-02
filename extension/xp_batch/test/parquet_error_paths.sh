#!/bin/bash
#
# Parquet provider: external resources on the error path, and interruptibility
#
#   extension/xp_batch/test/parquet_error_paths.sh <PGPORT> [PGHOST] [DB]
#
# Two properties that only matter when something goes wrong, which is why neither
# was covered by the arms that work.
#
# 1. An ERROR inside the pipeline longjmps past source->ops->end(). The Arrow
#    reader behind the Parquet provider is a C++ object holding an OS file
#    descriptor; it lives outside PostgreSQL's memory contexts, so nothing frees
#    it on unwind. Measured as the backend's open-descriptor count across
#    repeated failing queries in ONE session.
#
# 2. Arrow's call stack contains no CHECK_FOR_INTERRUPTS(), so a scan that is
#    inside the provider cannot be cancelled. Measured as the time a
#    statement_timeout of 10 ms actually takes to fire on a full-file scan.
#
# Both are measured as numbers rather than asserted as code properties, because
# the failure mode of the fix is that the callback is registered and never runs.
set -uo pipefail

PORT="${1:?port}"; PGHOST_ARG="${2:-}"; DB="${3:-testdb}"
PGBIN="${PGBIN:-/home/claude/pginstall20/bin}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
PQ_OK="${PQ_FILE:-$REPO/benchmarks/02-batch-joins/data/reg_buh.parquet}"
TMP="${TMPDIR:-/tmp}/xpb_error_paths.$$"

PSQL=("$PGBIN/psql" -p "$PORT" -d "$DB" -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

pass=0; fail=0
ok()   { echo "  ok      $1"; pass=$((pass+1)); }
bad()  { echo "  FAIL    $1"; fail=$((fail+1)); }

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

PRE=$("${PSQL[@]}" -c "SELECT 'alive'" 2>&1 | tail -1)
[ "$PRE" = "alive" ] || { echo "!! cannot query $DB on port $PORT: $PRE"; exit 2; }
PRE=$("${PSQL[@]}" -c "LOAD 'xpb_parquet'; SELECT 'loaded'" 2>&1 | tail -1)
[ "$PRE" = "loaded" ] || { echo "!! xpb_parquet will not load: $PRE"; exit 2; }
[ -f "$PQ_OK" ] || { echo "!! $PQ_OK absent (run benchmarks/02-batch-joins/export-parquet.py)"; exit 2; }
python3 -c "import pyarrow" 2>/dev/null || { echo "!! pyarrow not importable"; exit 2; }

mkdir -p "$TMP"
# A file the provider opens successfully and the pipeline then refuses: the four
# expected column names, with amount_dt as int64 where the pipeline reads int4.
# The point is an error raised AFTER the reader exists -- xpq_create closes its
# own reader before its own ereports, so those paths never leaked.
python3 - "$TMP/typemismatch.parquet" <<'PY'
import sys
import pyarrow as pa, pyarrow.parquet as pq
n = 1000
pq.write_table(pa.table({
    "period_key":  pa.array([25 + i % 12 for i in range(n)], type=pa.int32()),
    "company_key": pa.array([i % 50 for i in range(n)],      type=pa.int32()),
    "account_key": pa.array([1 + i % 200 for i in range(n)], type=pa.int32()),
    "amount_dt":   pa.array([i for i in range(n)],           type=pa.int64()),
}), sys.argv[1], row_group_size=500, compression="none", write_statistics=True)
PY
chmod -R a+rX "$TMP"

echo "=== 1. descriptors on the Parquet file after 10 failing queries ==="
# Counted by TARGET, not as a total: a backend opens relation files as it goes,
# so the total grows by a dozen whatever the provider does -- which is how the
# first version of this test managed to fail after the leak was fixed. One
# live reader holds exactly one descriptor on the file (verified by sampling
# during a scan), so descriptors on that path after the loop ARE leaked readers.
#
# The session stays alive on pg_sleep so the shell can read /proc while the
# backend is still there, and announces the end of the loop so the sample
# cannot land before it.
OUTF="$TMP/session.out"
"${PSQL[@]}" 2>/dev/null >"$OUTF" <<SQL &
LOAD 'xpb_parquet';
SET client_min_messages = error;
SELECT pg_backend_pid();
DO \$\$
DECLARE i int;
BEGIN
    FOR i IN 1..10 LOOP
        BEGIN
            PERFORM count(*) FROM xpb_batch_join2_groupby(25, 36,
                'ext:parquet:$TMP/typemismatch.parquet');
        EXCEPTION WHEN others THEN NULL;   -- the error is the point
        END;
    END LOOP;
END \$\$;
SELECT 'loopdone';
SELECT pg_sleep(4);
SQL
SESSION=$!

LEAKED=""
for _ in $(seq 1 50); do
    if grep -q loopdone "$OUTF" 2>/dev/null; then
        PID=$(head -1 "$OUTF")
        LEAKED=$(ls -l "/proc/$PID/fd" 2>/dev/null | grep -c "typemismatch.parquet")
        break
    fi
    sleep 0.2
done
kill "$SESSION" 2>/dev/null; wait "$SESSION" 2>/dev/null

if [ -z "$LEAKED" ]; then
    bad "the probe session never reached the end of the loop"
else
    echo "          descriptors still open on the file: $LEAKED (after 10 failed queries)"
    [ "$LEAKED" -eq 0 ] && ok "the reader is released on the error path" \
                        || bad "$LEAKED readers leaked, one per failed query"
fi

# And the error has to be the one expected, or the loop above proved nothing.
MSG=$("${PSQL[@]}" -c "LOAD 'xpb_parquet';
       SELECT count(*) FROM xpb_batch_join2_groupby(25,36,'ext:parquet:$TMP/typemismatch.parquet')" 2>&1 | tr '\n' ' ')
case "$MSG" in
    *int4*|*int8*|*type*) ok "the failing query fails for the expected reason" ;;
    *) bad "unexpected failure: $MSG" ;;
esac

echo "=== 2. statement_timeout on a full-file scan ==="
# Full range: every row group, so the backend is inside the provider for
# hundreds of ms. Without CHECK_FOR_INTERRUPTS in the batch loop the timeout
# cannot fire until the scan ends, and the measured elapsed time says which
# happened.
T0=$(date +%s%N)
OUT=$("${PSQL[@]}" -c "LOAD 'xpb_parquet'; SET statement_timeout = 10;
       SELECT count(*) FROM xpb_batch_join2_groupby(1,120,'ext:parquet:$PQ_OK')" 2>&1 | tr '\n' ' ')
T1=$(date +%s%N)
MS=$(( (T1 - T0) / 1000000 ))
echo "          elapsed to cancellation: ${MS} ms (statement_timeout 10 ms)"
case "$OUT" in
    *"canceling statement due to statement timeout"*) ok "the timeout fired" ;;
    *) bad "the timeout did not fire: $OUT" ;;
esac
# psql start, connect, LOAD and plan are inside this measurement, so the bound is
# generous; the full scan it replaces is several hundred ms.
[ "$MS" -lt 150 ] && ok "cancelled promptly (${MS} ms < 150 ms)" \
                  || bad "not interruptible inside the provider: ${MS} ms"

echo
echo "############ $pass correct, $fail wrong ############"
[ "$fail" -eq 0 ]
