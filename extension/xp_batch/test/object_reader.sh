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
  meta_calls bigint, data_calls bigint, irq_skipped_offthread bigint,
  short_reads_without_eof bigint, fault_attempts bigint, fault_injected bigint,
  logical_calls bigint, logical_bytes bigint)
LANGUAGE c AS '\$libdir/xpb_parquet', 'xpq_scan';
DROP FUNCTION IF EXISTS xpq_arm_fault(int,bigint,bigint,int);
CREATE FUNCTION xpq_arm_fault(mode int, at_read bigint, nbytes bigint, retries int)
RETURNS bool LANGUAGE c AS '\$libdir/xpb_parquet', 'xpq_arm_fault';"

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

echo "=== 7. the interrupt hook must not run on an Arrow thread ==="
#
# Arrow's pre_buffer dispatches column-chunk reads to arrow::internal::ThreadPool,
# so read_at() -- and therefore the interrupt hook -- is reached from a worker
# thread, not the backend's. Measured with pid/tid logged at the pread: the two
# footer reads arrive on the backend tid, every column chunk on a worker tid.
#
# That matters because CHECK_FOR_INTERRUPTS() is not thread-safe. Acting on it
# off-thread runs ProcessInterrupts() -> ereport -> siglongjmp into
# PG_exception_stack, which belongs to the backend thread, and on the FATAL path
# proc_exit() plus its exit handlers -- observed once as a worker running
# ThreadPool::Shutdown() on its own pool while the backend thread sat in
# ReadRangeCache::Read() waiting for that read. Unkillable backend, blocked
# cluster shutdown.
#
# The hook therefore fires only on the thread that installed it. The assertion
# below is not "no hook ran off-thread" on its own -- that would pass in a build
# where Arrow never used its pool, and the guard would be untested. It requires
# the off-thread case to HAVE OCCURRED and been skipped, which is what
# irq_skipped_offthread counts.
R=$(qall "SELECT read_calls||' '||meta_calls||' '||data_calls||' '||irq_skipped_offthread
          FROM xpq_scan('$PQ','$ALL',25,36)")
set -- $R
RC="${1:-0}"; MC="${2:-0}"; DC="${3:-0}"; SK="${4:-}"
if [ -z "$SK" ]; then
    bad "could not read irq_skipped_offthread: $R"
else
    echo "          reads $RC = meta $MC + data $DC; hook skipped off-thread: $SK"
    [ "$SK" -gt 0 ] && ok "the off-thread case occurs, so the guard is exercised ($SK reads)" \
                    || bad "no read arrived off-thread -- the guard is untested here, not proven unnecessary"
    # Every DATA read is pool-dispatched and every METADATA read is not, so the
    # skip count must equal the data reads exactly. An inequality would mean
    # some data read ran the hook on a worker, or a footer read did not run it.
    [ "$SK" = "$DC" ] && ok "skipped exactly the data reads ($SK = $DC), footer reads kept the check" \
                      || bad "skipped $SK but there were $DC data reads -- the split is not what it should be"
fi

# A cancel still has to arrive. It does not come from this layer on the data
# path any more; it comes from the backend thread's own reads and from the
# per-row-group CHECK_FOR_INTERRUPTS() in the provider. Section 5 measures the
# latency; this asserts an EXTERNAL signal mid-scan both arrives and does not
# wedge the backend, which is the failure that was actually observed.
for MODE in pg_cancel_backend pg_terminate_backend; do
    rm -f "$TMP/sig.out"
    "${PSQL[@]}" -c "SET client_min_messages=error; LOAD 'xpb_parquet';
        SELECT count(*) FROM generate_series(1,40) g,
               LATERAL xpb_batch_join2_groupby(1,120,'ext:parquet:$PQ') q" >"$TMP/sig.out" 2>&1 &
    SJ=$!
    SBP=""
    for _ in $(seq 1 60); do
        SBP=$("${PSQL[@]}" -c "SELECT pid FROM pg_stat_activity WHERE state='active'
              AND query LIKE '%generate_series(1,40)%' AND pid <> pg_backend_pid() LIMIT 1" 2>/dev/null | tail -1)
        [ -n "$SBP" ] && break
        sleep 0.05
    done
    if [ -z "$SBP" ]; then
        echo "  skip    $MODE -- the scan finished before it could be signalled"
        wait $SJ 2>/dev/null
        continue
    fi
    "${PSQL[@]}" -c "SELECT $MODE($SBP)" >/dev/null 2>&1
    W=0
    while kill -0 "$SBP" 2>/dev/null && [ $W -lt 40 ]; do sleep 0.25; W=$((W+1)); done
    wait $SJ 2>/dev/null
    if kill -0 "$SBP" 2>/dev/null; then
        bad "$MODE left backend $SBP alive after 10 s (threads=$(ls /proc/$SBP/task 2>/dev/null | wc -l))"
    else
        ok "$MODE mid-scan ended the backend in under $((W*250+250)) ms"
    fi
done

echo "=== 8. what a read means when the source is unreliable ==="
#
# v0 had one read_at() that was "exact except at end of object", so a caller
# inferred EOF from a short return. That inference is right for a local file --
# pread() returning 0 means the file ended -- and wrong for anything over a
# transport, where a ranged GET can come back short while the object continues.
# Under the v0 contract a dropped connection would therefore have arrived as a
# clean end of object: for a column store, not a crash but a silently short
# column.
#
# So EOF is no longer inferred, and the faults below are injected underneath the
# real Parquet reader -- same file, same bytes, same decode path, damaged
# delivery only -- so every answer stays checkable against the oracle.
#
# Modes: 1 short-without-eof, 2 transient-then-retry, 3 fail-after-n-bytes,
#        4 genuine EOF, 5 chunked delivery.
ONE=period_key,amount_dt
arm() { qall "DO \$\$ BEGIN PERFORM xpq_arm_fault($1,$2,$3,$4); END \$\$" >/dev/null 2>&1; }

# The armed fault is a static in the BACKEND, so arming and scanning have to be
# the same session -- two psql calls are two backends. Arming therefore happens
# inside a DO block, which returns no rows: an earlier version used a bare
# SELECT xpq_arm_fault(...) and its "t" became field 1 of the result, shifting
# every number by one and reporting the product as broken.
scanf() {   # scanf <mode> <at_read> <nbytes> <retries> <select-list>
    "${PSQL[@]}" -c "LOAD 'xpb_parquet'; SET client_min_messages=warning;
        DO \$\$ BEGIN PERFORM xpq_arm_fault($1,$2,$3,$4); END \$\$;
        SELECT $5 FROM xpq_scan('$PQ','$ONE',25,25)" 2>&1 | tr '\n' ' '
}

# -- the one that matters: a short read is not an end of object ---------------
OUT=$(scanf 1 3 1000 0 "rows")
case "$OUT" in
    *"short read with no end of object"*)
        ok "a short read with no EOF is refused, and says so in those words" ;;
    *ERROR*)
        bad "refused, but not as a short-read-without-EOF: $OUT" ;;
    *)
        bad "a truncated delivery was ACCEPTED -- this is the silent-short-column case: $OUT" ;;
esac

# -- and a real end of object is still a real end of object -------------------
# The control. If the fix were "distrust every short read" this would also fail,
# and the reader would be unable to tell the two apart -- which is the whole
# point of carrying eof explicitly.
OUT=$(scanf 4 3 1000 0 "rows")
case "$OUT" in
    *"read past end of object"*)
        ok "a genuine EOF is reported as EOF, distinguishably from a short read" ;;
    *"short read with no end of object"*)
        bad "a real EOF was misreported as a truncated transfer -- the two are not distinguished" ;;
    *ERROR*) bad "refused with the wrong reason: $OUT" ;;
    *)       bad "a read past the end was accepted: $OUT" ;;
esac

# -- bytes landing in the buffer before a failure must not be trusted --------
OUT=$(scanf 3 3 1000 0 "rows")
case "$OUT" in
    *"I/O error after"*) ok "a failure after partial delivery is an error, not a short answer" ;;
    *ERROR*)             bad "refused with the wrong reason: $OUT" ;;
    *)                   bad "a partly filled range was accepted: $OUT" ;;
esac

# -- retry: logical request unchanged, physical traffic changed --------------
BASE=$(scanf 0 0 0 0 "rows||' '||logical_calls||' '||logical_bytes||' '||read_calls||' '||bytes_requested")
RETRY=$(scanf 2 3 1000 2 "rows||' '||logical_calls||' '||logical_bytes||' '||read_calls||' '||bytes_requested")
set -- $BASE; BR="$1"; BLC="$2"; BLB="$3"; BPC="$4"; BPB="$5"
set -- $RETRY; RR="$1"; RLC="$2"; RLB="$3"; RPC="$4"; RPB="$5"
echo "          no fault: logical $BLC/$BLB B   physical $BPC/$BPB B   rows $BR"
echo "          retried:  logical $RLC/$RLB B   physical $RPC/$RPB B   rows $RR"
[ "$RR" = "$BR" ] && ok "a retried read returns the same rows ($RR)" \
                  || bad "retry changed the answer: $RR against $BR"
[ "$RLC" = "$BLC" ] && [ "$RLB" = "$BLB" ] \
    && ok "retry left the LOGICAL request untouched ($RLC calls / $RLB B)" \
    || bad "retry changed what Arrow asked for: $RLC/$RLB against $BLC/$BLB"
if [ "${RPC:-0}" -gt "${BPC:-0}" ]; then
    ok "retry cost physical traffic ($BPC -> $RPC calls, $BPB -> $RPB B)"
else
    bad "the retry is invisible in the physical counters ($RPC vs $BPC) -- it was free, so nothing was tested"
fi

# -- chunked delivery: same bytes, more calls, same answer -------------------
CH=$(scanf 5 3 1000 0 "rows||' '||logical_calls||' '||logical_bytes||' '||read_calls||' '||bytes_requested||' '||fault_injected")
set -- $CH; CR="$1"; CLC="$2"; CLB="$3"; CPC="$4"; CPB="$5"; CINJ="$6"
echo "          chunked:  logical $CLC/$CLB B   physical $CPC/$CPB B   rows $CR  injected $CINJ"
[ "${CINJ:-0}" -ge 1 ] && ok "the chunking fault fired ($CINJ)" \
                       || bad "no fault was injected, so this case tested nothing"
[ "$CR" = "$BR" ] && ok "a range delivered in pieces is reassembled whole ($CR rows)" \
                  || bad "chunked delivery changed the answer: $CR against $BR"
if [ "${CPC:-0}" -gt "${BPC:-0}" ] && [ "${CLB:-0}" = "${BLB:-0}" ]; then
    ok "same logical bytes, more physical calls ($BPC -> $CPC)"
else
    bad "chunking did not separate the levels: physical $CPC vs $BPC, logical $CLB vs $BLB"
fi

# -- corruption must never become a valid shortened column -------------------
# The query-level form of the same question, against the oracle checksum rather
# than a row count: under the faults that are recoverable the answer must be
# BIT-IDENTICAL, and under the faults that are not it must be an error.
GATE="md5(string_agg(year||','||account_group||','||company_key||','||total_amt,
       '|' ORDER BY year, account_group, company_key))||'|'||count(*)"
gate() {
    "${PSQL[@]}" -c "LOAD 'xpb_parquet'; SET client_min_messages=warning;
        DO \$\$ BEGIN PERFORM xpq_arm_fault($1,$2,$3,$4); END \$\$;
        SELECT $GATE FROM xpb_batch_join2_groupby(25,36,'ext:parquet:$PQ')" 2>&1 | tr '\n' ' '
}
trim() { echo $1; }
CLEAN=$(trim "$(gate 0 0 0 0)")
echo "          clean checksum: $CLEAN"
for M in 2 5; do
    G=$(trim "$(gate $M 5 4096 2)")
    [ "$G" = "$CLEAN" ] && ok "mode $M (recoverable): checksum identical" \
                        || bad "mode $M changed the answer: $G against $CLEAN"
done
for M in 1 3 4; do
    G=$(gate $M 5 4096 0)
    case "$G" in
        *ERROR*) ok "mode $M (unrecoverable): the query fails instead of returning short data" ;;
        *)       bad "mode $M returned an answer from damaged delivery: ${G##* }" ;;
    esac
done

# -- a fault-induced ERROR after open must not leak the descriptor -----------
# This is a REAL post-open error path, deterministically triggered. Before this
# phase the only post-open error available was a column name absent from the
# file, which fails before any data read.
rm -f "$TMP/fault.out"
"${PSQL[@]}" >"$TMP/fault.out" 2>&1 <<SQL &
LOAD 'xpb_parquet';
SET client_min_messages = notice;
SELECT pg_backend_pid();
DO \$\$
BEGIN
    FOR i IN 1..3 LOOP
        PERFORM xpq_arm_fault(1, 3, 1000, 0);
        BEGIN PERFORM count(*) FROM xpb_batch_join2_groupby(25,36,'ext:parquet:$PQ');
        EXCEPTION WHEN others THEN RAISE NOTICE 'refused'; END;
    END LOOP;
END \$\$;
SELECT 'faultdone';
SELECT pg_sleep(4);
SQL
FJ=$!
FBP=""; for _ in $(seq 1 100); do
    grep -q faultdone "$TMP/fault.out" 2>/dev/null && { FBP=$(head -1 "$TMP/fault.out"); break; }
    sleep 0.2
done
if [ -z "$FBP" ]; then
    bad "the fault probe session never finished"
else
    NREF=$(grep -c refused "$TMP/fault.out" 2>/dev/null); NREF=${NREF:-0}
    NFD=$(ls -l "/proc/$FBP/fd" 2>/dev/null | grep -c "$(basename "$PQ")" || true)
    echo "          $NREF fault-induced post-open errors, $NFD descriptors left"
    [ "$NREF" -eq 3 ] && ok "all three fault-induced post-open errors fired" \
                      || bad "expected 3 refusals, saw $NREF -- the rest of this case is vacuous"
    [ "$NFD" -eq 0 ] && ok "a fault-induced post-open ERROR leaks no descriptor" \
                     || bad "$NFD descriptor(s) left after fault-induced errors"
fi
kill "$FJ" 2>/dev/null; wait "$FJ" 2>/dev/null
[ -n "$FBP" ] && kill -0 "$FBP" 2>/dev/null && "${PSQL[@]}" -c "SELECT pg_terminate_backend($FBP)" >/dev/null 2>&1

# Leave nothing armed for whatever runs next.
arm 0 0 0 0

echo
echo "############ $pass correct, $fail wrong ############"
[ "$fail" -eq 0 ]
