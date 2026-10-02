#!/bin/bash
#
# The Parquet provider must be OPTIONAL in the strong sense.
#
#   extension/xp_batch/test/optional_module.sh <PGPORT> [PGHOST] [DB]
#
# xp_batch sits in shared_preload_libraries. If it referenced libparquet, a
# missing or broken Arrow installation would stop the cluster from starting
# rather than disable one feature. So the provider lives in a separate module
# and registers itself through xpb_source.h.
#
# This script asserts the properties that make that split real, rather than
# assuming them:
#
#   1. xp_batch.so names no Arrow/Parquet symbol at all.
#   2. The registry is empty until an optional module is loaded.
#   3. With xpb_parquet.so REMOVED from the filesystem, the server starts and
#      every existing XPBatch path still works.
#   4. LOAD of the absent module fails with a clear error and leaves the
#      backend usable.
#   5. With the module present, LOAD registers the provider and the existing
#      paths still work in the same backend.
#
# Property 3 is the one worth the trouble: it is checked by moving the module
# aside and restarting, not by reasoning about linkage.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${1:-5432}"; PGHOST_ARG="${2:-}"; DB="${3:-onec2}"
PGBIN="${PGBIN:-/home/claude/pginstall20/bin}"
PGDATA_DIR="${PGDATA_DIR:-/tmp/pgdata20}"
LIBDIR="$("$PGBIN/pg_config" --pkglibdir)"
MODULE="$LIBDIR/xpb_parquet.so"
XPBATCH="$LIBDIR/xp_batch.so"

# This script needs two identities: the server runs as a non-root user, while
# moving a module in and out of pkglibdir needs write access to it. SRVUSER is
# whoever owns the data directory; set it to "" to run psql/pg_ctl directly.
SRVUSER="${SRVUSER:-$(stat -c %U "$PGDATA_DIR" 2>/dev/null)}"
[ "$SRVUSER" = "$(id -un)" ] && SRVUSER=""

srv() {   # run a command as the server user, or directly when already it
    if [ -n "$SRVUSER" ]; then su "$SRVUSER" -c "PATH=$PGBIN:\$PATH $1"
    else env PATH="$PGBIN:$PATH" sh -c "$1"; fi
}
q() {     # one SQL string -> last output line
    local sql=${1//\"/\\\"}
    srv "psql -p $PORT ${PGHOST_ARG:+-h $PGHOST_ARG} -d $DB -X -qAt -c \"$sql\"" 2>&1
}

pass=0; fail=0
ok()   { echo "  ok      $1"; pass=$((pass+1)); }
bad()  { echo "  FAIL    $1"; fail=$((fail+1)); }
check(){ [ "$2" = "$3" ] && ok "$1 (= $3)" || bad "$1 — want [$2] got [$3]"; }

restart() { srv "pg_ctl -D $PGDATA_DIR -l /tmp/pg20.log restart -w -m fast" >/dev/null 2>&1; }

# Preflight. Without it every check below fails identically on one connection
# problem and the report blames the feature -- which is exactly what happened
# the first time this script ran.
if [ "$(q "SELECT 'probe'" | tail -1)" != "probe" ]; then
    echo "cannot connect as '${SRVUSER:-$(id -un)}': $(q "SELECT 'probe'" | tail -1)" >&2
    echo "usage: $0 <PGPORT> [PGHOST] [DB]   (SRVUSER=<server user> to override)" >&2
    exit 2
fi
if [ "$(q "SELECT count(*) FROM xpb_source_providers()" | tail -1)" = "" ]; then
    echo "xpb_source_providers() is not installed in $DB; run the extension SQL first" >&2
    exit 2
fi

#
# Steps 3 and 5 need a REAL XPBatch pipeline, not just a connection -- the
# point is that execution still works with the Arrow module absent, and a
# successful "SELECT 'up'" would not show that.
#
# Which pipeline depends on what this database holds. The script used to call
# xpb_v2_register_report unconditionally, which needs benchmark 05's reg2 /
# dim_company_d fixtures; against a database loaded from benchmark 02 it failed
# with "fact table or dimensions not found" and the report read as three
# product defects. So the pipeline is chosen from what is actually present, and
# if neither fixture set is there the two steps skip with a reason instead of
# failing.
PIPE_SQL=""; PIPE_SQL_Z=""; PIPE_WANT=""; PIPE_NAME=""
if [ "$(q "SELECT to_regclass('public.reg2') IS NOT NULL" | tail -1)" = "t" ]; then
    PIPE_NAME="v2 heap pipeline"
    PIPE_SQL="SELECT count(*) FROM xpb_v2_register_report(1,12,'heap-deform')"
    PIPE_SQL_Z="SELECT count(*) FROM xpb_v2_register_report(1,12,'zlfs')"
    PIPE_WANT=200
elif [ "$(q "SELECT to_regclass('public.reg_buh') IS NOT NULL" | tail -1)" = "t" ]; then
    PIPE_NAME="join2 heap pipeline"
    PIPE_SQL="SELECT count(*) FROM xpb_batch_join2_groupby(1,12,'heap')"
    PIPE_WANT=200
    #
    # The zlfs arm needs a BUILT zone for the slice it asks for; a hardcoded
    # [1..12] refused with "no zone -- build one with zlfs_build_zone()", which
    # is the source declining correctly and has nothing to do with whether the
    # Parquet module is present. So the slice comes from zlfs_zone_info(), and
    # the arm skips when no valid zone exists rather than building one -- this
    # script must not add zones to a benchmark database.
    ZSLICE=$(q "SET client_min_messages=error;
        SELECT period_lo||' '||period_hi FROM zlfs_zone_info() WHERE freshness='VALID' ORDER BY period_lo LIMIT 1" | tail -1)
    case "$ZSLICE" in
        [0-9]*" "[0-9]*)
            PIPE_SQL_Z="SELECT count(*) FROM xpb_batch_join2_groupby(${ZSLICE%% *},${ZSLICE##* },'zlfs')" ;;
        *) PIPE_SQL_Z="" ;;
    esac
fi
pipeline() {   # pipeline <label> <sql> <want>
    local out
    if [ -z "$PIPE_SQL" ]; then
        echo "  skip    $1 -- neither reg2 (bench 05) nor reg_buh (bench 02) is in $DB"
        return
    fi
    if [ -z "$2" ]; then
        echo "  skip    $1 -- no VALID zlfs zone in $DB; not building one here"
        return
    fi
    # Whole output, not tail -1: an ERROR's HINT or DETAIL is the last line, so
    # tail -1 reported "Build one with zlfs_build_zone()" as the answer and the
    # failure read as a wrong result rather than a refusal. This is the third
    # script in this suite to have had that bug.
    out=$(q "SET client_min_messages=warning; LOAD 'xp_batch'; $2" | tr '\n' ' ')
    case "$out" in
        *ERROR*) bad "$1 -- refused: $out" ;;
        *"$3"*)  ok  "$1 (= $3)" ;;
        *)       bad "$1 -- want [$3] got [$out]" ;;
    esac
}

echo "############ Parquet provider is an optional module ############"
echo

echo "=== 1. xp_batch.so must not reference Arrow or Parquet ==="
n=$(nm -D --undefined-only "$XPBATCH" 2>/dev/null | grep -icE "arrow|parquet" || true)
check "arrow/parquet symbols in xp_batch.so" "0" "$n"
echo

echo "=== 2. only the built-in providers with nothing optional loaded ==="
restart
# xp_batch registers heap, zlfs and pgcolumnar from its own _PG_init so that the
# LOOKUP is uniform; they are linked into xp_batch.so and are not optional. This
# used to assert an empty registry, which was true only while Parquet was the
# only provider. The property that matters is unchanged and is asserted below:
# nothing ARROW-dependent appears until its module is loaded.
n=$(q "SET client_min_messages=warning; SELECT string_agg(name, ',' ORDER BY name) FROM xpb_source_providers()" | tail -1)
check "built-in providers in a fresh backend" "heap,pgcolumnar,zlfs" "$n"
n=$(q "SET client_min_messages=warning; SELECT count(*) FROM xpb_source_providers() WHERE name = 'parquet'" | tail -1)
check "parquet absent until its module is loaded" "0" "$n"
echo

echo "=== 3. module REMOVED: server starts, existing paths work ==="
#
# This test renames the installed module out of the way and puts it back at
# step 5. If the script dies in between -- a timeout, a SIGTERM, a failing
# restart -- the module stays hidden and every later Parquet test in the suite
# fails with "could not access file", which looks like a product defect and is
# not one. It happened. So the restore is a trap, not a line at the end.
moved=0
unhide() {
    [ "$moved" = 1 ] && [ -f "$MODULE.hidden" ] && mv "$MODULE.hidden" "$MODULE"
    moved=0
}
trap 'unhide' EXIT HUP INT TERM
if [ -f "$MODULE" ]; then mv "$MODULE" "$MODULE.hidden" && moved=1; fi
restart
up=$(q "SELECT 'up'" | tail -1)
check "server accepts connections with the module absent" "up" "$up"

# A real XPBatch path, not just a connection.
pipeline "$PIPE_NAME with the module absent"      "$PIPE_SQL"   "$PIPE_WANT"
pipeline "$PIPE_NAME (zlfs) with the module absent" "$PIPE_SQL_Z" "$PIPE_WANT"
echo

echo "=== 4. LOAD of the absent module fails cleanly, backend survives ==="
out=$(q "SET client_min_messages=warning; LOAD 'xpb_parquet'")
if grep -q "could not (access|load) file\|could not access file\|could not load library" <<<"$out" \
   || grep -qE "could not (access|load)" <<<"$out"; then
    ok "LOAD reports the module is missing"
else
    bad "LOAD of an absent module — unexpected output: $(head -1 <<<"$out")"
fi
alive=$(q "SELECT 'alive'" | tail -1)
check "backend usable after the failed LOAD" "alive" "$alive"
echo

echo "=== 5. module restored: registers, and existing paths still work ==="
unhide
restart
got=$(q "SET client_min_messages=warning; LOAD 'xpb_parquet';
        SELECT name FROM xpb_source_providers()" | tail -1)
check "provider registers on LOAD" "parquet" "$got"

pipeline "$PIPE_NAME with the module loaded" "LOAD 'xpb_parquet'; $PIPE_SQL" "$PIPE_WANT"

echo
echo "############ $pass correct, $fail wrong ############"
exit $(( fail > 0 ? 1 : 0 ))
