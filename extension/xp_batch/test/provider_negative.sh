#!/bin/bash
#
# External source provider — the ten ways it can go wrong
#
#   extension/xp_batch/test/provider_negative.sh <PGPORT> [PGHOST] [DB]
#
# One matrix over every negative scenario an external provider can present, and
# the four things none of them may do:
#
#     no silent fallback      a declined or broken provider must not be replaced
#                             by another source behind the caller's back
#     no wrong result         and must not answer with somebody else's data
#     no fd/resource leak     measured, not argued
#     no backend hang         measured, not argued
#
# Two of these are measured in depth elsewhere and only checked for presence
# here: the descriptor-layout cases in test/provider_abi.sh, and the descriptor
# count and cancellation latency in test/parquet_error_paths.sh. This script is
# the matrix; those two are the instruments.
#
# Needs extension/xpb_abi_probe built and installed, and (for the source-side
# cases) benchmarks/02-batch-joins/data/reg_buh.parquet.
set -uo pipefail

PORT="${1:?port}"; PGHOST_ARG="${2:-}"; DB="${3:-testdb}"
PGBIN="${PGBIN:-/home/claude/pginstall20/bin}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
PQ="${PQ_FILE:-$REPO/benchmarks/02-batch-joins/data/reg_buh.parquet}"
TMP="${TMPDIR:-/tmp}/xpb_provider_negative.$$"

PSQL=("$PGBIN/psql" -p "$PORT" -d "$DB" -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

DECL="LOAD 'xpb_abi_probe';
CREATE OR REPLACE FUNCTION xpb_abi_register(text) RETURNS text
  LANGUAGE c AS '\$libdir/xpb_abi_probe','xpb_abi_register';
CREATE OR REPLACE FUNCTION xpb_abi_filters_rows(text) RETURNS text
  LANGUAGE c AS '\$libdir/xpb_abi_probe','xpb_abi_filters_rows';"
XPON="SET max_parallel_workers_per_gather=0; SET jit=off;"

pass=0; fail=0
ok()   { echo "  ok      $1"; pass=$((pass+1)); }
bad()  { echo "  FAIL    $1"; fail=$((fail+1)); }
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

qall() { "${PSQL[@]}" -c "$1" 2>&1 | tr '\n' ' '; }

# Every case must produce an ERROR mentioning something specific, and must not
# produce a row. "Refused" and "answered anyway" are the two outcomes that
# matter, so both are checked, not just the message.
refused() {  # refused <label> <needle> <sql>
    local out; out=$(qall "$3")
    case "$out" in
        *ERROR*) : ;;
        *) bad "$1 — no ERROR, output was [$out]"; return ;;
    esac
    case "$out" in
        *"$2"*) ok "$1" ;;
        *) bad "$1 — ERROR did not mention [$2]: $out" ;;
    esac
}

PRE=$("${PSQL[@]}" -c "SELECT 'alive'" 2>&1 | tail -1)
[ "$PRE" = "alive" ] || { echo "!! cannot query $DB on port $PORT: $PRE"; exit 2; }
PRE=$(qall "$DECL SELECT 'loaded'")
case "$PRE" in *loaded*) : ;; *) echo "!! xpb_abi_probe will not load: $PRE";
    echo "   build it: make -C extension/xpb_abi_probe install"; exit 2 ;; esac
mkdir -p "$TMP"

echo "=== registration side ==="
refused "1. a descriptor in the pre-abi_version layout"  "ABI version" "$DECL SELECT xpb_abi_register('legacy')"
refused "2. a descriptor shorter than the fixed prefix"  "struct_size" "$DECL SELECT xpb_abi_register('truncated')"
refused "3. an unknown ABI version"                      "ABI version" "$DECL SELECT xpb_abi_register('badversion')"
refused "4. the same provider name twice"                "already registered" "$DECL SELECT xpb_abi_register('duplicate')"

# Not a negative case, and it has to be here: if the accept path were broken the
# four refusals above would be worthless.
GOOD=$(qall "$DECL SELECT xpb_abi_register('current')")
case "$GOOD" in *registered*) ok "   and a well-formed descriptor is still accepted" ;;
    *) bad "   a well-formed descriptor was refused: $GOOD" ;; esac

echo "=== lookup side ==="
refused "5. a provider name that is not registered" "no source provider named" \
        "LOAD 'xpb_parquet'; $XPON SELECT count(*) FROM xpb_batch_join2_groupby(25,36,'ext:nosuch:/tmp/x.parquet')"
refused "6. a shared library that does not exist"   "could not" \
        "LOAD 'xpb_no_such_module'"
# A library that loads and registers nothing. xpb_abi_probe has no _PG_init, so
# loading it adds no provider -- the "installed but not registered" case, which
# must look like case 5 and not like a working provider.
refused "7. a library present whose load registered no provider" "no source provider named" \
        "LOAD 'xpb_abi_probe'; $XPON SELECT count(*) FROM xpb_batch_join2_groupby(25,36,'ext:abi_probe:/tmp/x.parquet')"

echo "=== source side ==="
refused "8. create() fails (file does not exist)" "cannot open" \
        "LOAD 'xpb_parquet'; $XPON SELECT count(*) FROM xpb_batch_join2_groupby(25,36,'ext:parquet:$TMP/absent.parquet')"

# next_batch() fails: a row group larger than the batch capacity. The provider
# refuses rather than handing over a short batch, which would be a silent row
# drop -- so this case exists to prove the refusal is still there.
python3 - "$TMP/big_rowgroup.parquet" <<'PY' 2>/dev/null || { echo "!! pyarrow unavailable, cases 9-10 skipped"; exit 2; }
import sys
import pyarrow as pa, pyarrow.parquet as pq
n = 70000                      # one row group > XPCB_BATCH_CAP (65 536)
pq.write_table(pa.table({
    "period_key":  pa.array([25 + i % 12 for i in range(n)], type=pa.int32()),
    "company_key": pa.array([i % 50 for i in range(n)],      type=pa.int32()),
    "account_key": pa.array([1 + i % 200 for i in range(n)], type=pa.int32()),
    "amount_dt":   pa.array([i % 1000 for i in range(n)],    type=pa.int32()),
}), sys.argv[1], row_group_size=n, compression="none", write_statistics=True)
PY
chmod -R a+rX "$TMP"
refused "9. next_batch() fails (row group exceeds batch capacity)" "batch capacity" \
        "LOAD 'xpb_parquet'; $XPON SELECT count(*) FROM xpb_batch_join2_groupby(25,36,'ext:parquet:$TMP/big_rowgroup.parquet')"

# A type the pipeline cannot read. Refused loudly rather than reinterpreted.
python3 - "$TMP/int8.parquet" <<'PY'
import sys
import pyarrow as pa, pyarrow.parquet as pq
n = 1000
pq.write_table(pa.table({
    "period_key":  pa.array([25 + i % 12 for i in range(n)], type=pa.int32()),
    "company_key": pa.array([i % 50 for i in range(n)],      type=pa.int32()),
    "account_key": pa.array([1 + i % 200 for i in range(n)], type=pa.int32()),
    "amount_dt":   pa.array(list(range(n)),                  type=pa.int64()),
}), sys.argv[1], row_group_size=500, compression="none", write_statistics=True)
PY
chmod -R a+rX "$TMP"
refused "10. a column whose type the operators cannot read" "int4" \
        "LOAD 'xpb_parquet'; $XPON SELECT count(*) FROM xpb_batch_join2_groupby(25,36,'ext:parquet:$TMP/int8.parquet')"

echo "=== and none of the above left the backend able to answer wrongly ==="
# The decisive check. After every failure above, the working arm must still give
# the gate's answer -- a provider that failed must not have changed what a later
# query returns, and must not have been quietly replaced by another source.
if [ -f "$PQ" ]; then
    GOT=$(qall "LOAD 'xpb_parquet'; $XPON SET client_min_messages=warning;
          SELECT md5(string_agg(year||','||account_group||','||company_key||','||total_amt,
                 '|' ORDER BY year, account_group, company_key))||'|'||count(*)
          FROM xpb_batch_join2_groupby(25,36,'ext:parquet:$PQ')")
    case "$GOT" in
        *"09e54f9a0108ff447c05f2cf63ca63da|200"*) ok "the working provider still returns the gate's answer" ;;
        *) bad "the working provider returned [$GOT]" ;;
    esac
else
    echo "  skip    gate re-check — $PQ absent"
fi

echo
echo "For the two instruments behind this matrix:"
echo "  descriptor layout, in depth   extension/xp_batch/test/provider_abi.sh"
echo "  leaks and cancellation        extension/xp_batch/test/parquet_error_paths.sh"
echo
echo "############ $pass correct, $fail wrong ############"
[ "$fail" -eq 0 ]
