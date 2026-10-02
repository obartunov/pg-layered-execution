#!/bin/bash
#
# No C++ exception may cross the C ABI
#
#   extension/xp_batch/test/parquet_cxx_boundary.sh <PGPORT> [PGHOST] [DB]
#
# The Parquet provider is C and its reader is C++. An exception leaving the C++
# half into C is undefined behaviour, not an error: PostgreSQL's ereport unwinds
# with longjmp, which does not run C++ destructors, and a C++ exception
# propagating through C frames is equally unspecified. The shim's rule is that
# every exported entry point catches everything and reports a return code.
#
# A malformed Parquet file does NOT test that rule -- Arrow reports those as a
# Status, so the catch arms are never reached. Both are checked here: the file
# cases for the status path, and a deliberate throw for the exception path.
#
#   kind 1   std::runtime_error            the typed catch arm
#   kind 2   parquet::ParquetException     Arrow's own, also a std::exception
#   kind 3   an int                        only catch (...) can take it
#
# The backend must still be usable after each one, which is the part that would
# fail if an exception really had crossed.
set -uo pipefail

PORT="${1:?port}"; PGHOST_ARG="${2:-}"; DB="${3:-testdb}"
PGBIN="${PGBIN:-/home/claude/pginstall20/bin}"
TMP="${TMPDIR:-/tmp}/xpb_cxx_boundary.$$"

PSQL=("$PGBIN/psql" -p "$PORT" -d "$DB" -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

DECL="LOAD 'xpb_parquet';
CREATE OR REPLACE FUNCTION xpq_selftest_throw(int) RETURNS text
  LANGUAGE c AS '\$libdir/xpb_parquet','xpq_selftest_throw_sql';"

pass=0; fail=0
ok()  { echo "  ok      $1"; pass=$((pass+1)); }
bad() { echo "  FAIL    $1"; fail=$((fail+1)); }
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

qall() { "${PSQL[@]}" -c "$1" 2>&1 | tr '\n' ' '; }

PRE=$("${PSQL[@]}" -c "SELECT 'alive'" 2>&1 | tail -1)
[ "$PRE" = "alive" ] || { echo "!! cannot query $DB on port $PORT: $PRE"; exit 2; }
PRE=$(qall "$DECL SELECT 'loaded'")
case "$PRE" in *loaded*) : ;; *) echo "!! xpb_parquet will not load: $PRE"; exit 2 ;; esac

echo "=== a deliberate throw, caught and converted ==="
# needle <label> <kind> <needle>
throws() {
    local out; out=$(qall "$DECL SELECT xpq_selftest_throw($2)")
    case "$out" in
        *"the boundary held"*)
            case "$out" in
                *"$3"*) ok "kind $2: $1" ;;
                *)      bad "kind $2 caught, but by the wrong arm: $out" ;;
            esac ;;
        *ERROR*) bad "kind $2 errored differently: $out" ;;
        *)       bad "kind $2 did not raise at all: $out" ;;
    esac
    # The decisive part: a backend that survived is a backend the exception did
    # not unwind through.
    local alive; alive=$("${PSQL[@]}" -c "SELECT 'alive'" 2>&1 | tail -1)
    [ "$alive" = "alive" ] && ok "kind $2: the backend is still usable" \
                           || bad "kind $2: the backend did not survive"
}

throws "std::exception caught by the typed arm"   1 "caught std::exception"
throws "ParquetException caught by the typed arm" 2 "caught std::exception"
throws "a non-std exception caught by catch (...)" 3 "caught non-std exception"

OUT=$(qall "$DECL SELECT xpq_selftest_throw(0)")
case "$OUT" in
    *"nothing thrown"*) ok "kind 0 throws nothing and returns normally" ;;
    *) bad "kind 0 should have returned normally: $OUT" ;;
esac

echo "=== and the status path, which is what a bad file actually takes ==="
mkdir -p "$TMP"; chmod 755 "$TMP"
head -c 200000 /dev/urandom > "$TMP/random.parquet"
{ printf 'PAR1'; head -c 300 /dev/urandom; printf 'PAR1'; } > "$TMP/badfooter.parquet"
chmod 644 "$TMP"/*.parquet

for f in random badfooter; do
    OUT=$(qall "LOAD 'xpb_parquet'; SET max_parallel_workers_per_gather=0;
          SELECT count(*) FROM xpb_batch_join2_groupby(25,36,'ext:parquet:$TMP/$f.parquet')")
    case "$OUT" in
        *"cannot open"*) ok "$f.parquet refused with Arrow's own diagnostic" ;;
        *ERROR*)         bad "$f.parquet errored differently: $OUT" ;;
        *)               bad "$f.parquet was ACCEPTED: $OUT" ;;
    esac
done
ALIVE=$("${PSQL[@]}" -c "SELECT 'alive'" 2>&1 | tail -1)
[ "$ALIVE" = "alive" ] && ok "the backend is still usable after both" \
                       || bad "the backend did not survive a malformed file"

echo
echo "############ $pass correct, $fail wrong ############"
[ "$fail" -eq 0 ]
