#!/bin/bash
#
# ObjectReader v0 -- the byte-range boundary under the Parquet reader
#
#   extension/xp_batch/test/object_reader.sh <PGPORT> [PGHOST] [DB]
#
# Three things, in this order of importance:
#
#   1. the architectural property: projection and pruning decide WHICH ranges
#      are fetched, and that shows up as fewer physical reads -- counted by the
#      ObjectReader, not derived from anything;
#   2. the failure semantics of a local byte source: missing, unreadable, not a
#      file, truncated, past EOF, double close;
#   3. cancellation between range reads.
#
# The counters come from the ObjectReader itself, which is the only layer that
# sees a physical read. bytes_requested and bytes_returned are separate because
# a short read is possible in principle (end of object); they are equal here,
# which is itself worth asserting -- it means the over-read that coalescing
# causes is Arrow asking for a WIDER range, not the reader returning less than
# it was asked for.
set -uo pipefail

PORT="${1:?port}"; PGHOST_ARG="${2:-}"; DB="${3:-testdb}"
PGBIN="${PGBIN:-/home/claude/pginstall20/bin}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
PQ="${PQ_FILE:-$REPO/benchmarks/02-batch-joins/data/reg_buh.parquet}"
TMP="${TMPDIR:-/tmp}/xpb_objreader.$$"

PSQL=("$PGBIN/psql" -p "$PORT" -d "$DB" -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

SCAN_DECL="DROP FUNCTION IF EXISTS xpq_scan(text,text,bigint,bigint);
CREATE FUNCTION xpq_scan(path text, cols text, lo bigint DEFAULT NULL, hi bigint DEFAULT NULL)
RETURNS TABLE (rows bigint, batches bigint, sum_last_col bigint, nulls_first_col bigint,
  row_groups_total int, row_groups_read int, row_groups_skipped int, decoded_values bigint,
  attributed_bytes bigint, copy_bytes bigint, arrow_chunks bigint,
  row_groups_stats_available int, row_groups_considered int, min_last_col bigint,
  max_last_col bigint, null_key_sum bigint, meta_bytes bigint, data_bytes bigint,
  read_calls bigint, decode_ms float8, bytes_requested bigint, bytes_returned bigint,
  meta_calls bigint, data_calls bigint)
LANGUAGE c AS '\$libdir/xpb_parquet', 'xpq_scan';"

pass=0; fail=0
ok()  { echo "  ok      $1"; pass=$((pass+1)); }
bad() { echo "  FAIL    $1"; fail=$((fail+1)); }
cleanup() { chmod -R u+rwX "$TMP" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

q()    { "${PSQL[@]}" -c "LOAD 'xpb_parquet'; SET client_min_messages=warning; $1" 2>&1 | tail -1; }
qall() { "${PSQL[@]}" -c "LOAD 'xpb_parquet'; SET client_min_messages=warning; $1" 2>&1 | tr '\n' ' '; }

PRE=$("${PSQL[@]}" -c "SELECT 'alive'" 2>&1 | tail -1)
[ "$PRE" = "alive" ] || { echo "!! cannot query $DB on port $PORT: $PRE"; exit 2; }
PRE=$(qall "SELECT 'loaded'")
case "$PRE" in *loaded*) : ;; *) echo "!! xpb_parquet will not load: $PRE"; exit 2 ;; esac
[ -f "$PQ" ] || { echo "!! $PQ absent (run benchmarks/02-batch-joins/export-parquet.py)"; exit 2; }
"${PSQL[@]}" -c "$SCAN_DECL" >/dev/null 2>&1 || { echo "!! cannot declare xpq_scan"; exit 2; }

ALL=period_key,company_key,account_key,amount_dt

# probe <cols> <lo> <hi> -> "rg_read skipped data_calls meta_calls data_bytes req ret"
probe() {
    local lo=${2:-NULL} hi=${3:-NULL}
    q "SELECT row_groups_read||' '||row_groups_skipped||' '||data_calls||' '||meta_calls
             ||' '||data_bytes||' '||bytes_requested||' '||bytes_returned
       FROM xpq_scan('$PQ','$1',$lo,$hi)"
}

echo "=== 1. projection and pruning decide the physical reads ==="
set -- $(probe "$ALL")
FULL_CALLS=$3; FULL_BYTES=$5
echo "          full scan, 4 of 8 columns: ${1} row groups, ${3} data reads, ${5} bytes"
set -- $(probe "$ALL" 25 36)
PRUNED_CALLS=$3; PRUNED_BYTES=$5
echo "          pruned to [25..36]:        ${1} row groups, ${3} data reads, ${5} bytes"
[ "$PRUNED_CALLS" -lt "$FULL_CALLS" ] && ok "pruning cut the physical reads ($FULL_CALLS -> $PRUNED_CALLS)" \
                                     || bad "pruning did not reduce reads: $FULL_CALLS -> $PRUNED_CALLS"

set -- $(probe "period_key" 25 36)
ONE_CALLS=$3; ONE_BYTES=$5
echo "          one column, same range:    ${1} row groups, ${3} data reads, ${5} bytes"
[ "$ONE_BYTES" -lt "$PRUNED_BYTES" ] && ok "projection cut the bytes ($PRUNED_BYTES -> $ONE_BYTES)" \
                                     || bad "projection did not reduce bytes"

# The decisive one: a row group that is pruned must cost NO read at all.
set -- $(probe "$ALL" 9999 99999)
echo "          pruned to nothing:         ${1} row groups, ${3} data reads, ${5} bytes"
if [ "$1" -eq 0 ] && [ "$3" -eq 0 ] && [ "$5" -eq 0 ]; then
    ok "a pruned row group costs no physical read"
else
    bad "pruned to nothing still read: ${3} calls, ${5} bytes"
fi
# and the metadata still costs what it costs
[ "$4" -eq 2 ] && ok "the footer still costs 2 metadata reads, counted not derived" \
               || bad "metadata reads: $4 (expected 2)"

echo "=== 2. requested equals returned, so over-read is a wider range ==="
set -- $(probe "$ALL" 25 36)
if [ "$6" = "$7" ]; then
    ok "bytes_requested == bytes_returned ($6) -- no short reads"
else
    bad "requested $6 != returned $7"
fi
# coalescing across a small gap asks for the gap too; that is over-read against
# what the footer attributes to the projection, not against what was returned.
OVER=$(q "SELECT data_bytes - attributed_bytes FROM xpq_scan('$PQ','period_key,account_key',25,36)")
CALLS=$(q "SELECT data_calls FROM xpq_scan('$PQ','period_key,account_key',25,36)")
echo "          period_key+account_key (gap 2 933): $CALLS data reads, over-read $OVER"
[ "$OVER" -gt 0 ] && ok "a small gap is coalesced and its bytes are fetched (over-read $OVER)" \
                  || bad "expected over-read from coalescing, got $OVER"
FAR=$(q "SELECT data_calls||' '||(data_bytes - attributed_bytes) FROM xpq_scan('$PQ','period_key,credit_key',25,36)")
echo "          period_key+credit_key (gap 51 808): $FAR"
case "$FAR" in
    *" 0") ok "a large gap is not coalesced and nothing extra is fetched" ;;
    *)     bad "expected no over-read across a large gap, got $FAR" ;;
esac

echo "=== 3. local-file failures, all through the one implementation ==="
mkdir -p "$TMP"; chmod 755 "$TMP"
cp "$PQ" "$TMP/ok.parquet"; chmod 644 "$TMP/ok.parquet"
head -c 4096 "$PQ" > "$TMP/truncated.parquet"; chmod 644 "$TMP/truncated.parquet"
cp "$PQ" "$TMP/denied.parquet"; chmod 000 "$TMP/denied.parquet"
mkdir -p "$TMP/adir.parquet"

refuses() {  # refuses <label> <path> <needle>
    local out; out=$(qall "SELECT rows FROM xpq_scan('$2','$ALL',25,36)")
    case "$out" in
        *ERROR*) case "$out" in
                     *"$3"*) ok "$1" ;;
                     *) bad "$1 -- refused for another reason: $out" ;;
                 esac ;;
        *) bad "$1 -- ACCEPTED: $out" ;;
    esac
}

refuses "a missing file"        "$TMP/nosuch.parquet"    "No such file"
refuses "permission denied"     "$TMP/denied.parquet"    "ermission denied"
refuses "a directory"           "$TMP/adir.parquet"      "not a regular file"
refuses "a truncated file"      "$TMP/truncated.parquet" "cannot open"

# Reading past the end is not an error: the footer probe does it on every open.
# A file smaller than the probe window proves the short read is tolerated.
OUT=$(qall "SELECT rows FROM xpq_scan('$TMP/ok.parquet','$ALL',25,36)")
case "$OUT" in
    *1050000*) ok "a good file still reads (1 050 000 rows) after all of the above" ;;
    *)         bad "the good file stopped working: $OUT" ;;
esac

echo "=== 4. no descriptor survives the source ==="
# Counted by target: the reader holds exactly one fd on the file while scanning.
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
            PERFORM rows FROM xpq_scan('$TMP/truncated.parquet','$ALL',25,36);
        EXCEPTION WHEN others THEN NULL;
        END;
        BEGIN
            PERFORM rows FROM xpq_scan('$TMP/ok.parquet','$ALL',25,36);
        EXCEPTION WHEN others THEN NULL;
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
        LEAKED=$(ls -l "/proc/$PID/fd" 2>/dev/null | grep -c "$TMP/" || true)
        break
    fi
    sleep 0.2
done
kill "$SESSION" 2>/dev/null; wait "$SESSION" 2>/dev/null
if [ -z "$LEAKED" ]; then
    bad "the probe session never finished its loop"
else
    echo "          descriptors still open under $TMP: $LEAKED"
    [ "$LEAKED" -eq 0 ] && ok "no file descriptor outlived the reader, over 10 good and 10 failed opens" \
                        || bad "$LEAKED descriptors leaked"
fi

echo "=== 5. cancellation between range reads ==="
T2=$(date +%s%N)
"${PSQL[@]}" -c "LOAD 'xpb_parquet'; SET client_min_messages=warning;
    SELECT count(*) FROM xpb_batch_join2_groupby(1,120,'ext:parquet:$PQ')" >/dev/null 2>&1
T3=$(date +%s%N); FULL=$(( (T3-T2)/1000000 ))
T0=$(date +%s%N)
OUT=$("${PSQL[@]}" -c "LOAD 'xpb_parquet'; SET statement_timeout = 10;
    SELECT count(*) FROM xpb_batch_join2_groupby(1,120,'ext:parquet:$PQ')" 2>&1 | tr '\n' ' ')
T1=$(date +%s%N); MS=$(( (T1-T0)/1000000 ))
echo "          uninterrupted ${FULL} ms, cancelled after ${MS} ms"
case "$OUT" in
    *"statement timeout"*) ok "the timeout fired" ;;
    *) bad "the timeout did not fire: $OUT" ;;
esac
if [ "$FULL" -lt 50 ]; then
    echo "  skip    the scan is too fast here to tell cancellation apart"
else
    [ "$MS" -lt $(( FULL / 2 )) ] && ok "cancelled early (${MS} ms against ${FULL} ms)" \
                                  || bad "not interruptible: ${MS} ms against ${FULL} ms"
fi

echo "=== 6. the descriptor's owner, one exit path at a time ==="
#
# Every way out of a scan, each one counted separately, because the previous
# section's aggregate "0 leaked" would also be satisfied by a path that never
# opened a reader. The reader is a C++ object holding an OS descriptor and is
# NOT in a PostgreSQL memory context, so each of these is a different mechanism:
#
#   normal end           xpq_end -> xpq_release
#   error inside open    the unique_ptr in xpq_open (this is what leaked)
#   error after open     ereport longjmp -> context reset -> xpq_context_cleanup
#   statement_timeout    the same, from the interrupt hook between range reads
#   pg_cancel_backend    the same, from another session
#   xpq_columns error    xpq_holder_cleanup, the non-source reader path
#   backend exit         the process, which is the only one that is free
#
# Counted inside the SAME backend after the exit, except for backend exit: a
# count taken after the process is gone proves nothing about the mechanism.
fdcount() {   # fds this backend holds on $PQ
    ls -l "/proc/$1/fd" 2>/dev/null | grep -c "$(basename "$PQ")" || true
}

PROBE="$TMP/probe.out"
PIPE="$TMP/probe.in"
rm -f "$PIPE"; mkfifo "$PIPE"
"${PSQL[@]}" -f "$PIPE" >"$PROBE" 2>&1 &
PSES=$!
exec 9>"$PIPE"
echo "LOAD 'xpb_parquet'; SET client_min_messages=error; SELECT pg_backend_pid();" >&9
BPID=""
for _ in $(seq 1 50); do BPID=$(head -1 "$PROBE" 2>/dev/null); [ -n "$BPID" ] && break; sleep 0.2; done
[ -n "$BPID" ] || { bad "the lifetime probe session never reported its pid"; BPID=0; }

step() {   # step <label> <sql>
    local label="$1" sql="$2" n
    : >"$TMP/mark"
    echo "$sql" >&9
    echo "SELECT 'step-done';" >&9
    for _ in $(seq 1 100); do
        n=$(grep -c "step-done" "$PROBE" 2>/dev/null); n=${n:-0}
        [ "$n" -ge "$STEPS" ] && break
        sleep 0.1
    done
    STEPS=$((STEPS+1))
    n=$(fdcount "$BPID")
    if [ "$n" -eq 0 ]; then ok "$label -- no descriptor held afterwards"
    else bad "$label -- $n descriptor(s) still held"; fi
}
STEPS=1

if [ "$BPID" != "0" ]; then
    # POSITIVE CONTROL, and the section is worthless without it: every
    # assertion below is "0 descriptors afterwards", which a reader that never
    # opened a file would also satisfy. So first catch the descriptor while it
    # exists -- sampled from HERE, while the scan is still running over there.
    #
    # The scan has to be one that holds the reader across many batches, which
    # is the provider path; xpq_scan materialises into a tuplestore and is gone
    # by the time it returns a row, so a cursor on it shows nothing and proves
    # nothing.
    echo "SELECT count(*) FROM xpb_batch_join2_groupby(1,120,'ext:parquet:$PQ');" >&9
    HELD=0
    for _ in $(seq 1 60); do
        HELD=$(fdcount "$BPID")
        [ "$HELD" -gt 0 ] && break
        sleep 0.05
    done
    echo "          sampled mid-scan: $HELD descriptor(s) on $(basename "$PQ")"
    [ "$HELD" -eq 1 ] && ok "the reader holds exactly one descriptor while scanning" \
                      || bad "expected 1 descriptor mid-scan, saw $HELD -- the rest of this section is vacuous"
    echo "SELECT 'step-done';" >&9
    for _ in $(seq 1 200); do
        n=$(grep -c step-done "$PROBE" 2>/dev/null); n=${n:-0}
        [ "$n" -ge 1 ] && break; sleep 0.1
    done
    STEPS=2

    step "normal end" \
         "SELECT count(*) FROM xpq_scan('$PQ','$ALL',25,36);"

    step "error inside xpq_open (truncated footer)" \
         "DO \$\$ BEGIN PERFORM rows FROM xpq_scan('$TMP/truncated.parquet','$ALL',25,36);
           EXCEPTION WHEN others THEN NULL; END \$\$;"

    step "error after open (column absent from the file)" \
         "DO \$\$ BEGIN PERFORM rows FROM xpq_scan('$PQ','no_such_column',25,36);
           EXCEPTION WHEN others THEN NULL; END \$\$;"

    step "statement_timeout during the scan" \
         "SET statement_timeout=10;
          DO \$\$ BEGIN PERFORM count(*) FROM xpb_batch_join2_groupby(1,120,'ext:parquet:$PQ');
           EXCEPTION WHEN others THEN NULL; END \$\$;
          SET statement_timeout=0;"

    # xpq_columns is the one reader NOT behind an XpBatchSource, so it has its
    # own holder. Two steps, and neither is the one I first wrote: a truncated
    # file makes xpq_open_held return NULL, so no reader exists and the holder
    # is never reached -- that step asserted nothing. What is reachable is the
    # normal release; the post-open ERROR window (CStringGetTextDatum,
    # tuplestore_putvalues) is reasoned from the code and is NOT exercised here.
    step "xpq_columns, successful call (holder normal release)" \
         "SELECT count(*) FROM xpq_columns('$PQ');"

    step "xpq_columns, open refused before any reader exists" \
         "DO \$\$ BEGIN PERFORM * FROM xpq_columns('$TMP/truncated.parquet');
           EXCEPTION WHEN others THEN NULL; END \$\$;"

    # pg_cancel_backend from outside, which is a different signal path than
    # statement_timeout even though both land in CHECK_FOR_INTERRUPTS.
    echo "SELECT count(*) FROM xpb_batch_join2_groupby(1,120,'ext:parquet:$PQ');" >&9
    sleep 0.25
    "${PSQL[@]}" -c "SELECT pg_cancel_backend($BPID)" >/dev/null 2>&1
    echo "SELECT 'step-done';" >&9
    for _ in $(seq 1 200); do
        n=$(grep -c step-done "$PROBE" 2>/dev/null); n=${n:-0}
        [ "$n" -ge "$STEPS" ] && break; sleep 0.1
    done
    STEPS=$((STEPS+1))
    CN=$(fdcount "$BPID")
    grep -q "canceling statement" "$PROBE" \
        && ok "pg_cancel_backend was delivered during the scan" \
        || bad "pg_cancel_backend never reached the scan"
    [ "$CN" -eq 0 ] && ok "pg_cancel_backend -- no descriptor held afterwards" \
                    || bad "pg_cancel_backend -- $CN descriptor(s) still held"
fi

# Close the write end, then make sure the session is actually gone. Closing
# the fifo did not always end it here, and a leftover idle backend blocks a
# smart shutdown -- which is how this harness stalled a later test run.
exec 9>&-
echo "\\q" > "$PIPE" 2>/dev/null &
for _ in $(seq 1 30); do kill -0 "$PSES" 2>/dev/null || break; sleep 0.1; done
kill -TERM "$PSES" 2>/dev/null
wait "$PSES" 2>/dev/null
[ "$BPID" != "0" ] && for _ in $(seq 1 30); do
    kill -0 "$BPID" 2>/dev/null || break
    sleep 0.1
done
if [ "$BPID" != "0" ] && kill -0 "$BPID" 2>/dev/null; then
    "${PSQL[@]}" -c "SELECT pg_terminate_backend($BPID)" >/dev/null 2>&1
    sleep 0.5
fi
# backend exit: the process is gone, so the descriptor is too. Asserted against
# the whole system rather than one backend, which is the only form of this
# question that is not vacuous.
sleep 0.5
STILL=$(ls -l /proc/[0-9]*/fd 2>/dev/null | grep -c "$(basename "$PQ")" || true)
[ "$STILL" -eq 0 ] && ok "after backend exit nothing on the host holds the file" \
                   || echo "  note    $STILL descriptor(s) elsewhere on the host (other sessions)"

echo
echo "############ $pass correct, $fail wrong ############"
[ "$fail" -eq 0 ]
