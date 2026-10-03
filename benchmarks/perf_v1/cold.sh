#!/bin/bash
#
# The cold state, which run.sh deliberately does not try to produce.
#
#   benchmarks/perf_v1/cold.sh <PGPORT> [PGHOST] [DB]
#
# Cold here means ALL THREE caches are empty, which takes more than a fresh
# connection:
#
#   shared_buffers   emptied by restarting the postmaster. A fresh backend does
#                    not do this -- shared_buffers is shared, so the pages the
#                    previous backend read are still there, and 1.25 GB of it
#                    holds the whole 575 MB fact table.
#   OS page cache    emptied by drop_caches. Without it the file still comes
#                    from RAM and "cold" measures a memcpy.
#   backend-local    emptied by being a new backend: the ZLFS zone registry
#                    lives in TopMemoryContext, and a Parquet reader's footer
#                    and HTTP connection are per-reader.
#
# One run per arm. A second would not be cold, and averaging two runs of
# different states is the thing this file exists to avoid.
#
# Only the selective shape [25..36] is measured cold. Cold runs cost minutes
# each on the heap arm, and the point of the cold column is the RATIO between
# representations on one shape, not a full cold matrix.
set -uo pipefail

PORT="${1:?port}"; PGHOST_ARG="${2:-/tmp/xpb_sock}"; DB="${3:-testdb}"
PGBIN="${PGBIN:-/home/claude/pginstall20/bin}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
PARQUET="$REPO/benchmarks/02-batch-joins/data/reg_buh.parquet"
S3URI="s3://xpb/reg_buh.parquet"
RESTART="${RESTART:-$HERE/pg-restart-for-cold.sh}"

COMMIT="$(cd "$REPO" && git rev-parse --short HEAD)"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
RUNDIR="$HERE/raw/$STAMP-$COMMIT-cold"
mkdir -p "$RUNDIR"
CSV="$HERE/summary/$STAMP-$COMMIT-cold.csv"

PSQL=("$PGBIN/psql" -p "$PORT" -h "$PGHOST_ARG" -d "$DB" -X -q)

LO=25; HI=36; WANT=1000008
ARMS_NAME=(heap-xpb pgcolumnar-xpb zlfs-xpb parquet-local-xpb parquet-s3-xpb)
ARMS_MODE=(heap pgcolumnar zlfs "ext:parquet:$PARQUET" "ext:parquet:$S3URI")

echo "arm,shape,state,run,wall_ms,total_ms,source_ms,operators_ms,rows,notes" > "$CSV"
echo "############ cold runs, shape selective [$LO..$HI] ############"
echo "raw: $RUNDIR"
echo

if [ ! -x "$RESTART" ]; then
    echo "!! need a restart helper at $RESTART that restarts the postmaster"
    echo "   with the XPB_S3_* environment this measurement needs."
    exit 2
fi

for i in "${!ARMS_NAME[@]}"; do
    arm="${ARMS_NAME[$i]}"; mode="${ARMS_MODE[$i]}"
    out="$RUNDIR/cold-$arm.txt"

    bash "$RESTART" > "$RUNDIR/.restart" 2>&1
    sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null \
        && dropped=yes || dropped="NO (not permitted -- this is not a cold run)"

    {
        echo "# arm=$arm shape=selective range=[$LO..$HI] state=cold commit=$COMMIT"
        echo "# postmaster restarted: yes    page cache dropped: $dropped"
        echo "# free after drop: $(free -m | awk '/^Mem:/{print $3" MB used, "$6" MB buff/cache"}')"
        echo "# command: psql -c \"SELECT count(*) FROM xpb_batch_join2_groupby($LO,$HI,'$mode')\""
        echo
    } > "$out"

    {
        echo "\\timing on"
        echo "SET client_min_messages=notice;"
        echo "LOAD 'xpb_parquet';"
        echo "SELECT count(*) AS groups, sum(total_amt) AS checksum FROM xpb_batch_join2_groupby($LO,$HI,'$mode');"
    } > "$RUNDIR/.q.sql"

    "${PSQL[@]}" -f "$RUNDIR/.q.sql" > "$RUNDIR/.o" 2>&1
    cat "$RUNDIR/.o" >> "$out"

    wall=$(grep -oP '^Time: \K[0-9.]+' "$RUNDIR/.o" | sed -n 3p)
    st=$(grep -oP 'total=\K.*' "$RUNDIR/.o" | head -1 | sed 's/^/total=/')
    f() { echo "$st" | grep -oP "(^|\s)$1=\K[0-9.]+" | head -1; }
    rows=$(f rows)

    # The oracle still gates the number: a cold run that returned the wrong
    # answer is not a slow measurement, it is not a measurement.
    if [ "$rows" != "$WANT" ]; then
        echo "  FAIL    $arm cold run returned rows=${rows:-none}, expected $WANT -- not recorded"
        continue
    fi
    printf '%s,selective,cold,1,%s,%s,%s,%s,%s,"page cache dropped: %s"\n' \
        "$arm" "${wall:-}" "$(f total)" "$(f source)" "$(f agg)" "$rows" "$dropped" >> "$CSV"
    printf '  %-20s cold  wall %10s ms   (total %s  source %s)\n' \
        "$arm" "${wall:-?}" "$(f total)" "$(f source)"
done

rm -f "$RUNDIR/.q.sql" "$RUNDIR/.o" "$RUNDIR/.restart"
echo
echo "summary csv: $CSV"
