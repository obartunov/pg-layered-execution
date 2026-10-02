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
moved=0
if [ -f "$MODULE" ]; then mv "$MODULE" "$MODULE.hidden" && moved=1; fi
restart
up=$(q "SELECT 'up'" | tail -1)
check "server accepts connections with the module absent" "up" "$up"

# A real XPBatch path, not just a connection: the v2 pipeline over the heap.
rows=$(q "SET client_min_messages=warning; LOAD 'xp_batch';
        SELECT count(*) FROM xpb_v2_register_report(1,12,'heap-deform')" | tail -1)
check "v2 heap pipeline with the module absent" "200" "$rows"

zrows=$(q "SET client_min_messages=warning; LOAD 'xp_batch';
        SELECT count(*) FROM xpb_v2_register_report(1,12,'zlfs')" | tail -1)
check "v2 zlfs pipeline with the module absent" "200" "$zrows"
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
[ "$moved" = 1 ] && mv "$MODULE.hidden" "$MODULE"
restart
got=$(q "SET client_min_messages=warning; LOAD 'xpb_parquet';
        SELECT name FROM xpb_source_providers()" | tail -1)
check "provider registers on LOAD" "parquet" "$got"

rows=$(q "SET client_min_messages=warning; LOAD 'xpb_parquet'; LOAD 'xp_batch';
        SELECT count(*) FROM xpb_v2_register_report(1,12,'heap-deform')" | tail -1)
check "v2 heap pipeline with the module loaded" "200" "$rows"

echo
echo "############ $pass correct, $fail wrong ############"
exit $(( fail > 0 ? 1 : 0 ))
