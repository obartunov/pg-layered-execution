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

echo
echo "############ $pass correct, $fail wrong ############"
[ "$fail" -eq 0 ]
