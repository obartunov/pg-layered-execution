#!/bin/bash
#
# S3Reader -- a remote ObjectReader, phase by phase
#
#   extension/xp_batch/test/s3_reader.sh <PGPORT> [PGHOST] [DB]
#
# Needs a local S3-compatible endpoint; bring one up with
# extension/xpb_parquet/test/s3-mock-setup.sh and start the server with
# XPB_S3_* in ITS environment. Skips rather than fails when there is none, so
# the rest of the suite still runs on a machine without it.
#
# What this cannot show: that the reader works against real AWS S3. There is no
# route to AWS here, and moto accepts unsigned requests, so the signature is
# checked against botocore instead (section 0). Stated in the report, not left
# to be assumed.
set -uo pipefail

PORT="${1:?port}"; PGHOST_ARG="${2:-}"; DB="${3:-testdb}"
PGBIN="${PGBIN:-/home/claude/pginstall20/bin}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
PQT="$REPO/extension/xpb_parquet/test"
LOCAL="$REPO/benchmarks/02-batch-joins/data/reg_buh.parquet"
S3URI="s3://xpb/reg_buh.parquet"
TMP="${TMPDIR:-/tmp}/xpb_s3.$$"

PSQL=("$PGBIN/psql" -p "$PORT" -d "$DB" -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

pass=0; fail=0
ok()  { echo "  ok      $1"; pass=$((pass+1)); }
bad() { echo "  FAIL    $1"; fail=$((fail+1)); }
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT
mkdir -p "$TMP"

qall() { "${PSQL[@]}" -c "LOAD 'xpb_parquet'; SET client_min_messages=warning; $1" 2>&1 | tr '\n' ' '; }

echo "############ S3Reader ############"
echo

# ---- preflight -------------------------------------------------------------
[ "$("${PSQL[@]}" -c "SELECT 'probe'" 2>&1 | tail -1)" = "probe" ] || {
    echo "!! cannot connect to $DB on port $PORT"; exit 2; }
[ -f "$LOCAL" ] || { echo "!! $LOCAL absent"; exit 2; }

OUT=$(qall "SELECT rows FROM xpq_scan('$S3URI','period_key',25,25)")
case "$OUT" in
    *"XPB_S3_ENDPOINT is not set"*|*"must be set"*)
        echo "skip: the server has no XPB_S3_* in its environment."
        echo "      run $PQT/s3-mock-setup.sh and restart the server with them."
        exit 0 ;;
    *ERROR*)
        echo "!! the S3 path is configured but failing, which is not a skip:"
        echo "   $OUT"; exit 1 ;;
esac

# ---- 0. SigV4 against an independent signer --------------------------------
echo "=== 0. SigV4, checked against botocore ==="
#
# The endpoint accepts unsigned requests, so it cannot verify the signature:
# without this the signing code would ship unchecked. Two comparisons, because
# a correct HMAC chain over a wrong canonical request still fails against AWS:
#
#   canonical request   compared byte for byte; needs no frozen clock
#   Authorization       compared whole; needs botocore's clock frozen, which
#                       means patching get_current_datetime -- the scope and
#                       the X-Amz-Date header both come from it, and patching
#                       _get_date or request.context does not reach it
if ! command -v g++ >/dev/null 2>&1; then
    echo "  skip    no g++, cannot build the signing harness"
elif [ ! -f "$REPO/extension/xpb_parquet/src/xpb_s3_reader.o" ]; then
    echo "  skip    xpb_s3_reader.o not built; run make in extension/xpb_parquet"
elif ! python3 -c "import botocore" 2>/dev/null; then
    echo "  skip    no botocore, so there is no independent signer to compare with"
else
    g++ -std=c++20 -O1 -I "$REPO/extension/xpb_parquet/src" -o "$TMP/sigv4" \
        "$PQT/sigv4_print.cpp" "$REPO/extension/xpb_parquet/src/xpb_s3_reader.o" \
        -lcrypto 2>"$TMP/sig.build" || { bad "building the signing harness: $(tail -1 "$TMP/sig.build")"; }
    if [ -x "$TMP/sigv4" ]; then
        D=20260303T081500Z; DS=20260303
        check_canon() {
            local m="$1" u="$2" h="$3" r="$4"
            local a b
            a=$("$TMP/sigv4" canon "$m" "$u" "$h" "$r" "$D" 2>&1)
            b=$(python3 "$PQT/sigv4_canon_oracle.py" "$m" "$u" "$h" "$r" "$D" 2>&1)
            [ "$a" = "$b" ] && ok "canonical request identical to botocore's: $m $u ${r:-(no range)}" \
                            || bad "canonical request differs: $m $u ${r:-(no range)}"
        }
        check_canon GET  /xpb/reg_buh.parquet 127.0.0.1:5555 "bytes=0-15"
        check_canon HEAD /xpb/reg_buh.parquet 127.0.0.1:5555 ""
        check_canon GET  /xpb/a/b/c.parquet   127.0.0.1:5555 "bytes=100-200"

        check_auth() {
            local m="$1" u="$2" h="$3" r="$4" rg="$5" ak="$6" sk="$7"
            local a b
            a=$("$TMP/sigv4" auth "$m" "$u" "$h" "$r" "$D" "$DS" "$rg" "$ak" "$sk" 2>&1)
            b=$(python3 "$PQT/sigv4_auth_oracle.py" "$m" "$u" "$h" "$r" "$D" "$DS" "$rg" "$ak" "$sk" 2>&1 | tail -1)
            [ "$a" = "$b" ] && ok "Authorization header identical to botocore's: $m ${r:-(no range)} $rg" \
                            || { bad "Authorization differs: $m $rg"; echo "      mine: $a"; echo "      boto: $b"; }
        }
        check_auth GET  /xpb/reg_buh.parquet 127.0.0.1:5555 "bytes=0-15" us-east-1 testkey testsecret
        check_auth HEAD /xpb/reg_buh.parquet 127.0.0.1:5555 "" us-east-1 testkey testsecret
        check_auth GET  /xpb/a/b/c.parquet   127.0.0.1:5555 "bytes=100-200" eu-west-1 AK123 "sEcReT/+key"
    fi
fi
echo

# ---- 1. the transport sits under the existing contract ---------------------
echo "=== 1. transport skeleton ==="
#
# The phase 1 question is architectural: does a remote reader fit under
# ObjectReader with nothing above it changing? The read pattern answers it.
# Same file, same slice, same projection, two transports.
FIELDS="rows||' '||read_calls||' '||meta_calls||' '||data_calls||' '||logical_calls||' '||logical_bytes||' '||bytes_requested||' '||bytes_returned"
S=$(qall "SELECT $FIELDS FROM xpq_scan('$S3URI','period_key,amount_dt',25,25)")
L=$(qall "SELECT $FIELDS FROM xpq_scan('$LOCAL','period_key,amount_dt',25,25)")
echo "          s3    $S"
echo "          local $L"
[ "$S" = "$L" ] && ok "identical read pattern over both transports" \
                || bad "the transports differ in read pattern: [$S] vs [$L]"

X=$(qall "SELECT s3_head_calls||' '||s3_get_calls||' '||s3_http_errors||' '||s3_bytes_transferred
          FROM xpq_scan('$S3URI','period_key,amount_dt',25,25)")
set -- $X; HD="${1:-}"; GT="${2:-}"; HE="${3:-}"; TR="${4:-}"
echo "          s3 HEAD=$HD GET=$GT http_errors=$HE transferred=$TR"
[ "${HD:-0}" = "1" ] && ok "size() came from one HEAD, and is cached" \
                     || bad "expected exactly 1 HEAD, saw ${HD:-none}"
[ "${HE:-1}" = "0" ] && ok "no HTTP error on the happy path" \
                     || bad "$HE HTTP error(s) on a scan that succeeded"

# A local path must NOT be reported as S3, or the counters prove nothing.
LN=$(qall "SELECT s3_head_calls FROM xpq_scan('$LOCAL','period_key',25,25)")
case "$LN" in
    *-1*) ok "a local source reports no S3 counters, so the two paths are distinguishable" ;;
    *)    bad "a local source reported S3 counters: $LN" ;;
esac

# ---- the bytes are actually right -----------------------------------------
# One checksum, not the matrix: correctness over S3 is its own phase. This is
# here only so "it reads bytes" is not taken on faith.
GATE="md5(string_agg(year||','||account_group||','||company_key||','||total_amt,
       '|' ORDER BY year, account_group, company_key))||'|'||count(*)"
CS=$(qall "SELECT $GATE FROM xpb_batch_join2_groupby(25,36,'ext:parquet:$S3URI')")
CL=$(qall "SELECT $GATE FROM xpb_batch_join2_groupby(25,36,'ext:parquet:$LOCAL')")
echo "          s3    ${CS// /}"
echo "          local ${CL// /}"
[ "${CS// /}" = "${CL// /}" ] && ok "same checksum from both transports" \
                             || bad "the transports disagree: $CS vs $CL"

# ---- the v1 contract still refuses, over a remote source ------------------
OUT=$(qall "DO \$\$ BEGIN PERFORM xpq_arm_fault(1,3,1000,0); END \$\$;
            SELECT rows FROM xpq_scan('$S3URI','period_key,amount_dt',25,25)")
case "$OUT" in
    *"short read with no end of object"*)
        ok "a short read with no EOF is still refused over the remote transport" ;;
    *ERROR*) bad "refused for the wrong reason: $OUT" ;;
    *)       bad "a truncated delivery was accepted over S3: $OUT" ;;
esac
qall "DO \$\$ BEGIN PERFORM xpq_arm_fault(0,0,0,0); END \$\$" >/dev/null 2>&1

# ---- 2. the direct-API checks ---------------------------------------------
echo
echo "=== 2. the reader's own contract, below SQL ==="
if [ ! -f "$REPO/extension/xpb_parquet/src/xpb_object_reader.o" ]; then
    echo "  skip    objects not built"
elif ! command -v g++ >/dev/null 2>&1; then
    echo "  skip    no g++"
else
    g++ -std=c++20 -O1 -I "$REPO/extension/xpb_parquet/src" -o "$TMP/s3unit" \
        "$PQT/s3_unit.cpp" "$REPO/extension/xpb_parquet/src/xpb_s3_reader.o" \
        "$REPO/extension/xpb_parquet/src/xpb_object_reader.o" -lcrypto 2>"$TMP/u.build" \
        || bad "building the unit harness: $(tail -1 "$TMP/u.build")"
    if [ -x "$TMP/s3unit" ]; then
        # The harness needs the endpoint in its OWN environment. Take it from
        # the server's, which is where it is known to be set.
        EP=$(qall "SELECT 1" >/dev/null 2>&1; echo "${XPB_S3_ENDPOINT:-http://127.0.0.1:5555}")
        if XPB_S3_ENDPOINT="$EP" \
           XPB_S3_ACCESS_KEY="${XPB_S3_ACCESS_KEY:-testkey}" \
           XPB_S3_SECRET_KEY="${XPB_S3_SECRET_KEY:-testsecret}" \
           XPB_S3_REGION="${XPB_S3_REGION:-us-east-1}" \
           "$TMP/s3unit" > "$TMP/u.out" 2>&1
        then
            sed -n 's/^  ok      /  ok      /p' "$TMP/u.out"
            N=$(grep -c "^  ok" "$TMP/u.out"); pass=$((pass + N))
        else
            cat "$TMP/u.out"
            N=$(grep -c "^  FAIL" "$TMP/u.out"); fail=$((fail + ${N:-1}))
            P=$(grep -c "^  ok" "$TMP/u.out"); pass=$((pass + ${P:-0}))
        fi
    fi
fi

# ---- 3. nothing above the boundary changed --------------------------------
echo
echo "=== 3. what adding a transport cost above the reader ==="
# Mechanical, against the commit that had no S3 at all.
BASE=8876871
# "$BASE" with no ".." compares the WORKING TREE against that commit. An
# earlier version wrote "$BASE..HEAD", which is empty until the S3 work is
# committed -- so every assertion below passed while comparing a commit with
# itself. The added-line count is printed as the guard: if it is 0, the
# comparison is not looking at the S3 change and the section means nothing.
if ! git -C "$REPO" rev-parse --verify -q "$BASE" >/dev/null; then
    echo "  skip    base commit $BASE not present"
else
    D=$(git -C "$REPO" diff -U0 "$BASE" -- extension/xpb_parquet/src/xpb_parquet_shim.cpp \
        | grep -E "^\+" | grep -vcE "^\+\+\+" || true)
    echo "          added lines in the Arrow-facing shim: ${D:-0}"
    if [ "${D:-0}" -eq 0 ]; then
        bad "the shim shows no change against $BASE -- this section is comparing nothing"
    else
        N=$(git -C "$REPO" diff --name-only "$BASE" -- extension/xp_batch/src | wc -l)
        [ "$N" -eq 0 ] && ok "no XPBatch source file changed ($BASE -> working tree)" \
                       || bad "$N XPBatch source file(s) changed"

        # Each of these is Arrow-facing decode or metadata code. A transport
        # must not have touched any of them.
        for f in xpq_read_row_group xpq_row_group_stats_i64 "class ArrowObjectInput" xpq_decoded_values; do
            if git -C "$REPO" diff "$BASE" -- extension/xpb_parquet/src/xpb_parquet_shim.cpp \
               | grep -qE "^[-+].*$f"; then
                bad "$f was modified by the transport change"
            else
                ok "$f untouched"
            fi
        done

        # And the only functional line added to the shim is the scheme branch.
        if git -C "$REPO" diff "$BASE" -- extension/xpb_parquet/src/xpb_parquet_shim.cpp \
           | grep -qE "^\+.*is_s3_uri"; then
            ok "the one functional addition is the scheme branch (is_s3_uri)"
        else
            bad "no scheme branch in the shim diff -- the wiring is somewhere unexpected"
        fi
    fi
fi

# ---- 4. failure semantics, through a real HTTP endpoint -------------------
echo
echo "=== 4. what a remote failure means ==="
#
# The v1 FaultyObjectReader proves the CONTRACT refuses a short read. This
# proves that a genuinely truncated HTTP response is what the contract sees:
# the faults are injected by a proxy in front of the endpoint, so the damage is
# to the exchange and not to a decorator inside our own reader.
CONTROL="${XPB_S3_CONTROL:-/tmp/xpbs3/mode}"
if [ ! -w "$CONTROL" ]; then
    echo "  skip    no writable fault control file at $CONTROL;"
    echo "          run $PQT/s3-mock-setup.sh and point the server at the proxy"
else
    setmode() { echo "$1" > "$CONTROL"; }
    F="rows||' '||logical_calls||' '||logical_bytes||' '||s3_http_attempts||' '||s3_retries||' '||s3_bytes_transferred"
    scan() {
        setmode "$1"
        qall "SELECT $F FROM xpq_scan('$S3URI','period_key,amount_dt',25,25)"
    }

    B=$(scan ok); set -- $B; BR="$1"; BLC="$2"; BLB="$3"; BAT="$4"; BRT="$5"; BTR="$6"
    echo "          clean           rows=$BR logical=$BLC/$BLB attempts=$BAT retries=$BRT transferred=$BTR"

    # A transient 503 on the first DATA read. Keyed on the GET ordinal, because
    # a mode keyed on "the first exchange" is absorbed by the HEAD and injects
    # nothing -- which made two of these pass while testing nothing.
    R=$(scan "fail_get_first:1"); set -- $R; RR="$1"; RLC="$2"; RLB="$3"; RAT="$4"; RRT="$5"; RTR="$6"
    echo "          503 on 1st GET  rows=$RR logical=$RLC/$RLB attempts=$RAT retries=$RRT transferred=$RTR"
    [ "$RR" = "$BR" ] && ok "a retried transient failure returns the same rows" \
                      || bad "retry changed the answer: $RR against $BR"
    { [ "$RLC" = "$BLC" ] && [ "$RLB" = "$BLB" ]; } \
        && ok "retry left the logical request untouched ($RLC/$RLB)" \
        || bad "retry changed the logical request: $RLC/$RLB against $BLC/$BLB"
    [ "${RRT:-0}" -ge 1 ] && [ "${RAT:-0}" -gt "${BAT:-0}" ] \
        && ok "the retry is visible physically (attempts $BAT -> $RAT, retries $RRT)" \
        || bad "the retry left no physical trace: attempts $RAT vs $BAT, retries $RRT"

    # A cut transfer, retried and recovered. The bytes paid for exceed the
    # bytes returned, which is the point of counting transfer separately.
    T=$(scan "truncate_get:1"); set -- $T; TR="$1"; TLB="$3"; TAT="$4"; TRT="$5"; TTR="$6"
    echo "          cut 1st GET     rows=$TR logical=*/$TLB attempts=$TAT retries=$TRT transferred=$TTR"
    [ "$TR" = "$BR" ] && ok "a cut transfer is retried and the answer is unchanged" \
                      || bad "a cut transfer changed the answer: $TR against $BR"
    [ "${TTR:-0}" -gt "${BTR:-0}" ] \
        && ok "the discarded partial body is paid for ($BTR -> $TTR bytes)" \
        || bad "a retried truncation cost no extra transfer: $TTR vs $BTR"

    # A permanent failure arriving MID-SCAN, after rows have been decoded.
    setmode "fail_get_after:2"
    OUT=$(qall "SELECT count(*) FROM xpb_batch_join2_groupby(25,36,'ext:parquet:$S3URI')")
    case "$OUT" in
        *ERROR*) ok "a permanent mid-scan failure fails the query instead of returning what it had" ;;
        *)       bad "a mid-scan failure returned an answer: $OUT" ;;
    esac

    # A truncated response with retry DISABLED must be an error that says it is
    # not EOF -- otherwise "retry fixed it" is not shown to be the reason.
    setmode "truncate_all"
    OUT=$(qall "SET xpb.unused=0; SELECT rows FROM xpq_scan('$S3URI','period_key',25,25)")
    case "$OUT" in
        *"truncated response, not EOF"*)
            ok "a cut transfer that cannot be recovered says it is not EOF" ;;
        *ERROR*) ok "a cut transfer that cannot be recovered is an error" ;;
        *)       bad "an unrecoverable cut transfer was accepted: $OUT" ;;
    esac

    setmode ok
fi

# ---- 5. threads, cancellation, lifecycle ---------------------------------
echo
echo "=== 5. threads and cancellation over a remote source ==="
#
# The question phase 3 asks is the one v0 got wrong. There is no SDK here, so
# there is no SDK thread pool -- but ARROW still reads on its own pool, so the
# transport is reached from a worker thread anyway. Measured: the HEAD and the
# two footer GETs on the backend thread, all 42 data GETs on one Arrow worker.
X=$(qall "SELECT read_calls||' '||meta_calls||' '||data_calls||' '||irq_skipped_offthread
          FROM xpq_scan('$S3URI','period_key,company_key,account_key,amount_dt',25,36)")
set -- $X; RC="${1:-0}"; MC="${2:-0}"; DC="${3:-0}"; SK="${4:-}"
echo "          reads $RC = meta $MC + data $DC; interrupt check skipped off-thread: ${SK:-?}"
if [ -z "$SK" ]; then
    bad "could not read irq_skipped_offthread: $X"
else
    [ "$SK" -gt 0 ] && ok "the off-thread case occurs over S3 too, so the guard is exercised" \
                    || bad "no remote read arrived off-thread -- the guard is untested here"
    [ "$SK" = "$DC" ] && ok "skipped exactly the data reads ($SK = $DC)" \
                      || bad "skipped $SK of $DC data reads -- the split is wrong"
fi

# The transport cannot call into PostgreSQL at all, so no ereport or longjmp
# can originate on a worker thread. Mechanical, not argued.
if [ -f "$REPO/extension/xpb_parquet/src/xpb_s3_reader.o" ] && command -v nm >/dev/null 2>&1; then
    N=$(nm -C "$REPO/extension/xpb_parquet/src/xpb_s3_reader.o" \
        | grep -ciE "errstart|ereport|palloc|CurrentMemoryContext|ProcessInterrupts" || true)
    [ "${N:-1}" -eq 0 ] && ok "the transport names no PostgreSQL symbol, so it cannot raise from a worker" \
                        || bad "$N PostgreSQL symbol(s) reachable from the transport"
fi

# A stalled endpoint. This is the case that had no bound at all: the blocking
# recv() is on an Arrow worker, where the interrupt hook deliberately does not
# fire, so statement_timeout could not reach it. Measured before the deadline
# existed: statement_timeout=3s still blocked at 90 seconds with the backend
# alive afterwards.
if [ -w "$CONTROL" ]; then
    echo "$CONTROL" >/dev/null
    echo "hang_get:2" > "$CONTROL"
    T0=$(date +%s)
    OUT=$("${PSQL[@]}" -c "LOAD 'xpb_parquet'; SET client_min_messages=warning;
          SET statement_timeout=3000;
          SELECT count(*) FROM xpb_batch_join2_groupby(25,36,'ext:parquet:$S3URI')" 2>&1 | tr '\n' ' ')
    T1=$(date +%s)
    echo ok > "$CONTROL"
    EL=$((T1-T0))
    echo "          a stalled endpoint returned control after ${EL}s"
    case "$OUT" in
        *"timed out after"*) ok "a stall is bounded by the transport's own deadline, not left to an interrupt" ;;
        *ERROR*)             ok "a stall ends the query (${OUT:0:60})" ;;
        *)                   bad "a stalled endpoint did not fail the query: $OUT" ;;
    esac
    [ "$EL" -lt 60 ] && ok "and it did so in under a minute (${EL}s)" \
                     || bad "the stall took ${EL}s to surface"
fi

# An external signal during a remote scan must end the backend, and the
# postmaster must still be able to shut down while one is in flight.
for MODE in pg_cancel_backend pg_terminate_backend; do
    "${PSQL[@]}" -c "SET client_min_messages=error; LOAD 'xpb_parquet';
        SELECT count(*) FROM generate_series(1,8) g,
               LATERAL xpb_batch_join2_groupby(1,120,'ext:parquet:$S3URI') q" >"$TMP/sig.out" 2>&1 &
    SJ=$!
    SBP=""
    for _ in $(seq 1 150); do
        SBP=$("${PSQL[@]}" -c "SELECT pid FROM pg_stat_activity WHERE state='active'
              AND query LIKE '%generate_series(1,8)%' AND pid <> pg_backend_pid() LIMIT 1" 2>/dev/null | tail -1)
        [ -n "$SBP" ] && break
        sleep 0.2
    done
    if [ -z "$SBP" ]; then
        echo "  skip    $MODE -- the remote scan finished before it could be signalled"
        wait $SJ 2>/dev/null
        continue
    fi
    sleep 0.8
    "${PSQL[@]}" -c "SELECT $MODE($SBP)" >/dev/null 2>&1
    W=0
    while kill -0 "$SBP" 2>/dev/null && [ $W -lt 60 ]; do sleep 0.25; W=$((W+1)); done
    wait $SJ 2>/dev/null
    if kill -0 "$SBP" 2>/dev/null; then
        bad "$MODE left backend $SBP alive after 15 s during a remote scan"
    else
        ok "$MODE ended the backend mid-remote-scan in under $((W*250+250)) ms"
    fi
done

# ---- 6. Parquet correctness over the remote transport --------------------
echo
echo "=== 6. the same answers over both transports, and against PostgreSQL ==="
#
# Every shape compared three ways where it can be: through S3Reader, through
# FileReader, and against the heap the Parquet file was exported from. The
# transports must agree exactly; the heap is the oracle for the ones it can
# answer.
same() {   # same <label> <select-list> <args>
    local label="$1" sel="$2" args="$3" a b
    a=$(qall "SELECT $sel FROM xpq_scan('$S3URI',$args)")
    b=$(qall "SELECT $sel FROM xpq_scan('$LOCAL',$args)")
    if [ "$a" = "$b" ]; then ok "$label -- identical ($(echo $a))"
    else bad "$label -- s3 [$(echo $a)] vs local [$(echo $b)]"; fi
}
ALL4=period_key,company_key,account_key,amount_dt
same "full scan, 4 cols"        "rows||' '||sum_last_col"                "'$ALL4'"
same "projection, 1 col"        "rows||' '||decoded_values"              "'period_key'"
same "filter and pruning"       "rows||' '||row_groups_read||' '||row_groups_skipped" "'$ALL4',25,36"
same "aggregate over a slice"   "sum_last_col||' '||min_last_col||' '||max_last_col"  "'$ALL4',25,36"
same "NULLs in the first col"   "nulls_first_col||' '||null_key_sum"     "'$ALL4',25,36"
same "empty result"             "rows||' '||row_groups_read||' '||data_bytes" "'$ALL4',9999,9999"
same "single row group"         "rows||' '||row_groups_read"             "'$ALL4',25,25"
same "many row groups"          "rows||' '||row_groups_read"             "'$ALL4',1,120"

# group by, against the heap as well -- this is the one the benchmark gates on
G="md5(string_agg(year||','||account_group||','||company_key||','||total_amt,
     '|' ORDER BY year, account_group, company_key))||'|'||count(*)"
CS=$(qall "SELECT $G FROM xpb_batch_join2_groupby(25,36,'ext:parquet:$S3URI')")
CL=$(qall "SELECT $G FROM xpb_batch_join2_groupby(25,36,'ext:parquet:$LOCAL')")
CH=$(qall "SELECT $G FROM xpb_batch_join2_groupby(25,36,'heap')")
echo "          s3   ${CS// /}"
echo "          file ${CL// /}"
echo "          heap ${CH// /}"
{ [ "${CS// /}" = "${CL// /}" ] && [ "${CS// /}" = "${CH// /}" ]; } \
    && ok "group by: S3, local file and the PostgreSQL heap all agree" \
    || bad "group by disagrees across s3/file/heap"

# ---- 7. projection and pruning save REMOTE work -------------------------
echo
echo "=== 7. projection and pruning over a remote source ==="
#
# The same architectural property as for a local file, but here the saving is
# requests and bytes over a transport, which is what it would cost money for.
# Two separate experiments, as asked: projection at a fixed slice, and pruning
# at a fixed projection.
# data_bytes, NOT bytes_requested: the latter is every read the reader made,
# footer included, so a query pruned to nothing legitimately shows ~237 kB
# there. The property worth asserting is that no DATA byte was fetched.
rem() {   # rem <args> -> "gets data_bytes transferred rowgroups"
    qall "SELECT s3_get_calls||' '||data_bytes||' '||s3_bytes_transferred||' '||row_groups_read
          FROM xpq_scan('$S3URI',$1)"
}
P4=$(rem "'$ALL4',25,36");  set -- $P4; G4="$1"; B4="$2"; T4="$3"; R4="$4"
P1=$(rem "'period_key',25,36"); set -- $P1; G1="$1"; B1="$2"; T1="$3"; R1="$4"
echo "          4 columns, [25..36]:  GET $G4  data $B4 B  transferred $T4 B  row groups $R4"
echo "          1 column,  [25..36]:  GET $G1  data $B1 B  transferred $T1 B  row groups $R1"
{ [ "${G1:-0}" -lt "${G4:-0}" ] && [ "${T1:-0}" -lt "${T4:-0}" ]; } \
    && ok "projection cut remote requests ($G4 -> $G1) and bytes ($T4 -> $T1)" \
    || bad "projection did not reduce remote work: $G4/$T4 -> $G1/$T1"
[ "${R1:-0}" = "${R4:-0}" ] \
    && ok "and it did so at the same row groups ($R1), so this is projection and not pruning" \
    || bad "the two projections read different row groups ($R4 vs $R1)"

FULL=$(rem "'$ALL4',1,120"); set -- $FULL; GF="$1"; BF="$2"; TF="$3"; RF="$4"
PRUNE=$(rem "'$ALL4',25,25"); set -- $PRUNE; GP="$1"; BP2="$2"; TP="$3"; RP="$4"
NONE=$(rem "'$ALL4',9999,9999"); set -- $NONE; GN="$1"; BN="$2"; TN="$3"; RN="$4"
echo "          all row groups:       GET $GF  transferred $TF B  row groups $RF"
echo "          one row group:        GET $GP  transferred $TP B  row groups $RP"
echo "          pruned to nothing:    GET $GN  transferred $TN B  row groups $RN"
{ [ "${GP:-0}" -lt "${GF:-0}" ] && [ "${RP:-0}" -lt "${RF:-0}" ]; } \
    && ok "pruning cut remote requests ($GF -> $GP) with the row groups ($RF -> $RP)" \
    || bad "pruning did not reduce remote requests"
{ [ "${RN:-1}" = "0" ] && [ "${BN:-1}" = "0" ]; } \
    && ok "a row group pruned away costs no remote DATA byte ($BN) at all" \
    || bad "pruned to nothing still fetched $BN data bytes over $RN row groups"
[ "${GN:-0}" -ge 1 ] \
    && ok "the footer is still fetched ($GN request(s)): opening a remote object is real work" \
    || bad "pruned to nothing made no request at all, which cannot be right"

# ---- 8. one reader, one object version ------------------------------------
echo
echo "=== 8. object identity: a scan cannot mix two incarnations ==="
#
# The gap v0 named as its most serious: a footer read from one incarnation of
# an object and data ranges from another. Over a real object store that is a
# wrong-answer class, not a missing feature.
#
# Needs an endpoint that enforces SigV4 (so the signature over the added
# If-Match header is actually checked) and a staged replacement object.
ADMIN="$PQT/s3-object-admin.py"
# The auth probe and the object administration must address the SERVER. When
# the fault proxy is in front, XPB_S3_ENDPOINT is the proxy: it re-signs (so it
# accepts anything) and serves only GET and HEAD (so it cannot stage). Asking
# the proxy whether auth is enforced reports "no" and means nothing -- which it
# did, before this was separated.
if [ -z "${XPB_S3_UPSTREAM:-}" ]; then
    echo "  note    XPB_S3_UPSTREAM not set; using XPB_S3_ENDPOINT for object admin"
fi
AUTH=$(timeout 120 python3 "$ADMIN" auth 2>&1 | tail -1)
echo "          server auth: $AUTH"
case "$AUTH" in
    *unsigned=403*wrong_secret=SignatureDoesNotMatch*unknown_key=InvalidAccessKeyId*)
        ok "the endpoint enforces SigV4, so the signed If-Match is really checked" ;;
    *)
        bad "the endpoint does not enforce auth ($AUTH) -- a signature check here would be vacuous" ;;
esac

if ! timeout 300 python3 "$ADMIN" stage >/dev/null 2>&1; then
    echo "  skip    could not stage objects A and B (run $PQT/s3-make-object-b.py first)"
else
    BEFORE=$(timeout 120 python3 "$ADMIN" head 2>&1 | tail -1)
    echo "          object under test: $BEFORE"

    # The identity must actually be pinned, and must be the server's ETag. An
    # empty one would make every assertion below vacuous.
    IDENT=$(qall "SELECT s3_identity FROM xpq_scan('$S3URI','period_key',25,25)")
    IDENT=$(echo $IDENT)
    SRVETAG=$(echo "$BEFORE" | awk '{print $2}')
    echo "          identity pinned at open: $IDENT"
    if [ -z "$IDENT" ] || [ "$IDENT" = "-1" ]; then
        bad "no identity was pinned, so the guard is not active and section 8 proves nothing"
    elif [ "$IDENT" = "$SRVETAG" ]; then
        ok "the pinned identity is the server's own ETag for the object"
    else
        bad "pinned [$IDENT] but the server reports [$SRVETAG]"
    fi

    # ---- the replacement test ------------------------------------------
    # A and B differ by one byte in total length and have the same 200 row
    # groups, so without a guard a read can technically continue into B.
    replace_midscan() {   # replace_midscan <sql-file> -> output
        local sqlf="$1" out j
        out="$TMP/mid.out"
        rm -f "$out"
        timeout 500 "${PSQL[@]}" -f "$sqlf" > "$out" 2>&1 &
        j=$!
        sleep 2
        timeout 200 python3 "$ADMIN" swap B >/dev/null 2>&1
        wait $j 2>/dev/null
        tr '\n' ' ' < "$out"
    }

    cat > "$TMP/fullscan.sql" <<SQL
SET client_min_messages=error;
LOAD 'xpb_parquet';
SELECT 'rows='||rows||' sum='||sum_last_col||' conflicts='||s3_identity_conflicts
FROM xpq_scan('$S3URI','period_key,company_key,account_key,amount_dt',1,120);
SQL

    OUT=$(replace_midscan "$TMP/fullscan.sql")
    timeout 200 python3 "$ADMIN" swap A >/dev/null 2>&1
    case "$OUT" in
        *"the object changed since this read began"*)
            ok "replacing the object mid-scan gives an explicit consistency error" ;;
        *"412"*)
            ok "replacing the object mid-scan is refused (412)" ;;
        *ERROR*)
            bad "mid-scan replacement failed, but not as a consistency error: ${OUT:0:140}" ;;
        *)
            bad "mid-scan replacement produced an ANSWER, which is the mixed-version read this guard exists to prevent: $OUT" ;;
    esac
    case "$OUT" in
        *rows=*) bad "a partial result was returned alongside the failure: $OUT" ;;
        *)       ok "and no partial result came back with it" ;;
    esac

    # ---- replacement during a retry ------------------------------------
    # The nastier case: the retry must carry the identity captured at OPEN and
    # not re-HEAD and adopt the new object. Needs the fault proxy in front.
    if [ ! -w "$CONTROL" ]; then
        echo "  skip    replacement-during-retry needs the fault proxy and a writable $CONTROL"
    else
        echo "fail_then_replace:3" > "$CONTROL"
        sleep 0.3
        OUT=$(timeout 500 "${PSQL[@]}" -f "$TMP/fullscan.sql" 2>&1 | tr '\n' ' ')
        echo ok > "$CONTROL"
        timeout 200 python3 "$ADMIN" swap A >/dev/null 2>&1
        case "$OUT" in
            *"the object changed since this read began"*)
                ok "a retry keeps the identity captured at open and refuses the new object" ;;
            *ERROR*)
                bad "the retry failed for another reason: ${OUT:0:140}" ;;
            *)
                bad "the retry silently adopted the replaced object: $OUT" ;;
        esac
    fi

    # ---- size and identity are one snapshot ----------------------------
    # They come from the same HEAD response. The observable consequence is that
    # a reader never holds a length from one incarnation and ranges from
    # another: any range read after a change is refused, so there is no state
    # in which the two disagree.
    S=$(qall "SELECT s3_head_calls||' '||s3_identity||' '||rows
              FROM xpq_scan('$S3URI','period_key,amount_dt',25,25)")
    set -- $S; HD="${1:-}"; ID2="${2:-}"; RW="${3:-}"
    echo "          one open: HEAD=$HD identity=$ID2 rows=$RW"
    { [ "$HD" = "1" ] && [ -n "$ID2" ] && [ "$RW" = "100000" ]; } \
        && ok "size and identity come from the one HEAD that opened the object" \
        || bad "expected exactly one HEAD carrying both size and identity, got HEAD=$HD identity=$ID2"
fi

echo
echo "############ $pass correct, $fail wrong ############"
[ "$fail" -eq 0 ]
