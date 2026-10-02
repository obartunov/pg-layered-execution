#!/bin/bash
#
# XpbSourceProvider ABI compatibility
#
#   extension/xp_batch/test/provider_abi.sh <PGPORT> [PGHOST] [DB]
#
# The descriptor crosses a module boundary and nothing in the build system
# rebuilds the two modules together, so "both were rebuilt" is an assumption a
# deployment can break. This asserts what happens when it is broken.
#
# The case that motivates the test is `legacy`: a descriptor in the layout that
# predates abi_version, which is what a provider .so built before the fix has in
# its text. Before the fix it registered, and the bool that decides WHO APPLIES
# THE PREDICATE was read out of the low bytes of the old describe pointer:
#
#   BEFORE FIX: legacy descriptor -> registered
#   BEFORE FIX: filters_rows of abi_legacy -> true
#
# filters_rows = true means the caller skips the range filter, so that mismatch
# returns rows outside the predicate -- a wrong answer from a stale .so, with no
# failed load and no error. The fix refuses it instead.
#
# `current`, `minimal`, `badversion` and `future` are here so "refused" cannot be
# satisfied by refusing everything, and so the zero-fill that makes a short
# descriptor safe is observed rather than argued.
#
# How easy the mismatch is to produce, from this fix's own development: PGXS on
# this host does not track header dependencies, so editing xpb_source.h and
# running make rebuilt only the module whose .c had changed. xp_batch kept the
# old registry, xpb_parquet passed a new-layout descriptor, and the old registry
# read abi_version (0x58500001) as the name pointer -- strcmp on that address
# crashed the backend. After a header change, make clean in every module.
#
# Needs the test module built and installed:
#   make -C extension/xpb_abi_probe && make -C extension/xpb_abi_probe install
set -uo pipefail

PORT="${1:?port}"; PGHOST_ARG="${2:-}"; DB="${3:-testdb}"
PGBIN="${PGBIN:-/home/claude/pginstall20/bin}"

q() {   # one SQL string -> last output line, errors included
    "$PGBIN/psql" -p "$PORT" ${PGHOST_ARG:+-h "$PGHOST_ARG"} -d "$DB" -X -qAt \
        -c "$1" 2>&1 | tail -1
}
qall() { # one SQL string -> ALL output on one line. Refusals are judged on the
         # ERROR and DETAIL lines, and tail -1 lands on the HINT.
    "$PGBIN/psql" -p "$PORT" ${PGHOST_ARG:+-h "$PGHOST_ARG"} -d "$DB" -X -qAt \
        -c "$1" 2>&1 | tr '\n' ' '
}

DECL="LOAD 'xpb_abi_probe';
CREATE OR REPLACE FUNCTION xpb_abi_register(text) RETURNS text
  LANGUAGE c AS '\$libdir/xpb_abi_probe','xpb_abi_register';
CREATE OR REPLACE FUNCTION xpb_abi_filters_rows(text) RETURNS text
  LANGUAGE c AS '\$libdir/xpb_abi_probe','xpb_abi_filters_rows';"

pass=0; fail=0
ok()   { echo "  ok      $1"; pass=$((pass+1)); }
bad()  { echo "  FAIL    $1"; fail=$((fail+1)); }
check(){ [ "$2" = "$3" ] && ok "$1 (= $3)" || bad "$1 — want [$2] got [$3]"; }
has()  { case "$3" in *"$2"*) ok "$1";; *) bad "$1 — [$2] not in [$3]";; esac; }

# Preflight. Without it every case below fails identically on one connection or
# packaging problem and the report blames the ABI.
PRE=$(q "SELECT 'alive'")
[ "$PRE" = "alive" ] || { echo "!! cannot query $DB on port $PORT: $PRE"; exit 2; }
PRE=$(q "$DECL SELECT 'loaded'")
[ "$PRE" = "loaded" ] || { echo "!! xpb_abi_probe will not load: $PRE"; echo "   build it: make -C extension/xpb_abi_probe install"; exit 2; }

echo "=== 1. a descriptor in the pre-abi_version layout is refused ==="
# Each case runs in its own backend: the registry is per-backend and append-only.
has "legacy refused, and the message names the ABI version" "ABI version" \
    "$(qall "$DECL SELECT xpb_abi_register('legacy')")"
# The decisive one. A refusal that still left the provider in the table would be
# worse than none: the caller looks providers up by name, not by return value.
# The error is swallowed in plpgsql so the same backend can then be asked --
# and a subtransaction abort does not undo a C static array write, which is
# exactly the state under test.
check "and it is NOT in the registry afterwards" "absent" \
      "$(q "$DECL
            DO \$\$ BEGIN PERFORM xpb_abi_register('legacy');
                   EXCEPTION WHEN others THEN NULL; END \$\$;
            SELECT xpb_abi_filters_rows('abi_legacy')")"

echo "=== 2. a well-formed current descriptor is accepted, value intact ==="
check "current registers"              "registered" "$(q "$DECL SELECT xpb_abi_register('current')")"
check "filters_rows survives the copy" "true"       "$(q "$DECL SELECT xpb_abi_register('current'); SELECT xpb_abi_filters_rows('abi_current')")"

echo "=== 3. a descriptor that predates the appended field reads false ==="
# struct_size stops before filters_rows, and the provider sets it to true inside
# its own struct. The registry must not read that byte: it is outside the size
# the provider declared, and false is the direction where the caller filters.
check "minimal registers"                  "registered" "$(q "$DECL SELECT xpb_abi_register('minimal')")"
check "filters_rows zero-filled, not read" "false"      "$(q "$DECL SELECT xpb_abi_register('minimal'); SELECT xpb_abi_filters_rows('abi_minimal')")"

echo "=== 4. version and size mismatches are refused, not ignored ==="
has "wrong abi_version refused"  "ABI version" "$(qall "$DECL SELECT xpb_abi_register('badversion')")"
has "oversized struct refused"   "struct_size" "$(qall "$DECL SELECT xpb_abi_register('future')")"

echo "=== 5. the real provider still registers and still works ==="
# Asked through xpb_find_source_provider rather than xpb_source_providers(), so
# this checks registration AND the field the pipeline reads, in one answer, and
# does not depend on which databases have the extension's SQL up to date.
check "parquet registers, filters_rows false" "false" \
      "$(q "LOAD 'xpb_parquet'; $DECL SELECT xpb_abi_filters_rows('parquet')")"

PQ="${PQ_FILE:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/benchmarks/02-batch-joins/data/reg_buh.parquet}"
if [ -f "$PQ" ]; then
    # End to end, because the registry change is only interesting if the path it
    # feeds still produces the right answer.
    check "ext:parquet arm still returns the gate's answer" \
          "09e54f9a0108ff447c05f2cf63ca63da|200" \
          "$(q "LOAD 'xpb_parquet'; SET max_parallel_workers_per_gather=0; SET jit=off;
                SELECT md5(string_agg(year||','||account_group||','||company_key||','||total_amt,
                       '|' ORDER BY year, account_group, company_key))||'|'||count(*)
                FROM xpb_batch_join2_groupby(25,36,'ext:parquet:$PQ')")"
else
    echo "  skip    ext:parquet end-to-end — $PQ absent (run benchmarks/02-batch-joins/export-parquet.py)"
fi

echo
echo "############ $pass correct, $fail wrong ############"
[ "$fail" -eq 0 ]
