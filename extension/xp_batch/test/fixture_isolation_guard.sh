#!/bin/bash
#
# The test suite must not change the benchmark dataset
#
#   extension/xp_batch/test/fixture_isolation_guard.sh <PGPORT> [PGHOST] [DB]
#
# Several C entry points resolve fixture names themselves -- RelnameGetRelid
# ("reg_buh"), dim_period, dim_account -- so tests that exercise them have to
# build tables under those names. Which means a test pointed at the benchmark
# database can destroy a 10M-row dataset, and one did.
#
# This takes a census of the benchmark tables, runs the suite, and takes it
# again. Existence and row counts must match. It asserts the property the
# individual fixes are supposed to produce, rather than trusting that each test
# isolates itself.
#
# Isolation as it stands:
#   heap_layout_guard.sh     creates and drops its own database
#   silent_drop_map.sh       creates and drops its own database
#   regression_guards.sh     CREATE TEMP TABLE, shadowing via the temp schema
#   reproducers (groupagg2*) own fixture names (r13_*, r14_*, r15_*), dropped
#   provider_* / parquet_*   read-only against the dataset, or own temp files
#   source_conformance.sh    read-only; CREATE TEMP TABLE for its own fixtures
#   object_reader.sh         read-only against the Parquet file; own temp dir,
#                            and it terminates its own probe backend -- a
#                            leftover idle session blocks a smart shutdown,
#                            which stalled optional_module.sh once
set -uo pipefail

PORT="${1:?port}"; PGHOST_ARG="${2:-}"; DB="${3:-testdb}"
PGBIN="${PGBIN:-/home/claude/pginstall20/bin}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PSQL=("$PGBIN/psql" -p "$PORT" -d "$DB" -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

census() {
    "${PSQL[@]}" -c "
      SELECT string_agg(t || '=' || n, ' ' ORDER BY t) FROM (
        SELECT 'reg_buh' t,     coalesce((SELECT count(*) FROM public.reg_buh), -1) n
        WHERE to_regclass('public.reg_buh') IS NOT NULL
        UNION ALL
        SELECT 'dim_period',    coalesce((SELECT count(*) FROM public.dim_period), -1)
        WHERE to_regclass('public.dim_period') IS NOT NULL
        UNION ALL
        SELECT 'dim_account',   coalesce((SELECT count(*) FROM public.dim_account), -1)
        WHERE to_regclass('public.dim_account') IS NOT NULL
        UNION ALL
        SELECT 'reg_buh_col',   coalesce((SELECT count(*) FROM public.reg_buh_col), -1)
        WHERE to_regclass('public.reg_buh_col') IS NOT NULL
      ) s" 2>&1 | tail -1
}

PRE=$("${PSQL[@]}" -c "SELECT 'alive'" 2>&1 | tail -1)
[ "$PRE" = "alive" ] || { echo "!! cannot query $DB on port $PORT: $PRE"; exit 2; }

BEFORE=$(census)
if [ -z "$BEFORE" ]; then
    echo "!! $DB holds none of the benchmark tables -- nothing to guard."
    echo "   Point this at the database benchmarks/common/load.sh populated."
    exit 2
fi
echo "before: $BEFORE"

# Every test that touches the database, with the arguments the suite uses.
# Deliberately INCLUDING the two that build benchmark-named fixtures.
run() { echo "  running $1"; ( cd "$HERE" && timeout 900 bash "$@" ) >/dev/null 2>&1 || true; }

run reproducers/silent_drop_map.sh      "$PORT" "$PGHOST_ARG"
run heap_layout_guard.sh                "$PORT" "$PGHOST_ARG"
run regression_guards.sh                "$PORT" "$PGHOST_ARG" "$DB"
run contract_tests.sh                   "$PORT" "$PGHOST_ARG"
run reproducers/groupagg2_zlfs_zone_mismatch.sh        "$PORT" "$PGHOST_ARG" "$DB"
run reproducers/groupagg2_attr_offset_and_null_pred.sh "$PORT" "$PGHOST_ARG" "$DB"
run reproducers/groupagg2_desc_stream_exit.sh          "$PORT" "$PGHOST_ARG"
run reproducers/local_ht_silent_drop.sh                "$PORT" "$PGHOST_ARG"
run provider_abi.sh                     "$PORT" "$PGHOST_ARG" "$DB"
run provider_negative.sh                "$PORT" "$PGHOST_ARG" "$DB"
run parquet_error_paths.sh              "$PORT" "$PGHOST_ARG" "$DB"
run parquet_cxx_boundary.sh             "$PORT" "$PGHOST_ARG" "$DB"
run source_conformance.sh               "$PORT" "$PGHOST_ARG" "$DB"
run object_reader.sh                    "$PORT" "$PGHOST_ARG" "$DB"

AFTER=$(census)
echo "after:  $AFTER"

if [ "$BEFORE" = "$AFTER" ]; then
    echo
    echo "############ benchmark fixtures unchanged ############"
    exit 0
fi
echo
echo "!! the suite CHANGED the benchmark fixtures"
echo "############ FAIL ############"
exit 1
