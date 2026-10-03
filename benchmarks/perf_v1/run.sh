#!/bin/bash
#
# Performance Baseline v1 -- the cost map of layered execution at one commit.
#
#   benchmarks/perf_v1/run.sh <PGPORT> [PGHOST] [DB]
#
# NOT an optimization run. Nothing here is tuned to make a number look better;
# see docs/PERFORMANCE_BASELINE_V1.md, section "What this does not prove".
#
# ---------------------------------------------------------------- WALL TIME
#
# Wall time is taken from psql's own \timing, never from the total_ms the
# modules report. They are different quantities, and the difference is large:
# the first zlfs query in a fresh backend measured 159.9 ms of wall time while
# reporting total=6.9 ms, because the 16 MB zone load happens in the provider's
# create(), outside every stage counter. total_ms is the instrumented region
# only -- it excludes source construction (zone load, file open, footer read)
# and result materialization.
#
# So each run records both, and `wall - total` is reported as UNINSTRUMENTED,
# never as the time of a layer. Where that gap matters it is measured directly
# instead: the zlfs zone load is the difference between the first and second
# call in one backend, which is a measurement of two wall times and not a
# subtraction of two counters.
#
# -------------------------------------------------------------- WARM / COLD
#
# Three states, kept apart because they answer different questions:
#
#   cold    PostgreSQL restarted AND the OS page cache dropped, fresh backend.
#           Nothing is in shared_buffers, nothing in page cache, no zone loaded.
#           One run only: a second one would not be cold.
#   newconn warm OS and shared_buffers, but a FRESH BACKEND. This is the state
#           that exposes per-backend source construction -- the zlfs zone load,
#           a reopened Parquet file, a new HTTP connection.
#   warm    same backend, after a warm-up run. Five measured runs.
#
# Mixing them into one median would hide exactly the thing the planner would
# need to know, so they are never averaged together.
set -uo pipefail

PORT="${1:?port}"; PGHOST_ARG="${2:-/tmp/xpb_sock}"; DB="${3:-testdb}"
PGBIN="${PGBIN:-/home/claude/pginstall20/bin}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
PARQUET="$REPO/benchmarks/02-batch-joins/data/reg_buh.parquet"
S3URI="s3://xpb/reg_buh.parquet"

RAW="$HERE/raw"
SUM="$HERE/summary"
mkdir -p "$RAW" "$SUM"

COMMIT="$(cd "$REPO" && git rev-parse --short HEAD)"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
RUNDIR="$RAW/$STAMP-$COMMIT"
mkdir -p "$RUNDIR"
CSV="$SUM/$STAMP-$COMMIT.csv"

PSQL=("$PGBIN/psql" -p "$PORT" -h "$PGHOST_ARG" -d "$DB" -X -q)

# Measured runs per warm series. Five, per the task.
NWARM="${NWARM:-5}"

# ------------------------------------------------------------------ environment
env_record() {
    local f="$RUNDIR/00-environment.txt"
    {
        echo "commit:        $COMMIT"
        echo "stamp:         $STAMP"
        echo "host:          $(uname -srm)  cpus=$(nproc)"
        echo "memory:        $(free -m | awk '/^Mem:/{print $2" MB total"}')"
        echo "pg:            $("${PSQL[@]}" -At -c 'select version()')"
        echo "pgbin:         $PGBIN"
        echo "port/host/db:  $PORT $PGHOST_ARG $DB"
        echo
        echo "-- PostgreSQL settings that bear on this measurement"
        "${PSQL[@]}" -At -c "select '  '||name||' = '||setting||coalesce(unit,'')
            from pg_settings
            where name in ('shared_buffers','work_mem','maintenance_work_mem',
                           'effective_cache_size','max_parallel_workers',
                           'max_parallel_workers_per_gather','jit','io_method',
                           'seq_page_cost','random_page_cost','huge_pages',
                           'track_io_timing')
            order by name"
        echo
        echo "-- extensions"
        "${PSQL[@]}" -At -c "select '  '||extname||' '||extversion from pg_extension order by 1"
        echo
        echo "-- dataset identity (the SAME logical rows in every representation)"
        "${PSQL[@]}" -At -c "select '  heap   reg_buh:      rows='||count(*)||'  sum(amount_dt)='||sum(amount_dt)||'  period_key='||min(period_key)||'..'||max(period_key) from reg_buh"
        "${PSQL[@]}" -At -c "select '  pgcol  reg_buh_col:  rows='||count(*)||'  sum(amount_dt)='||sum(amount_dt) from reg_buh_col"
        "${PSQL[@]}" -At -c "select '  size   '||relname||' = '||pg_size_pretty(pg_total_relation_size(oid)) from pg_class where relname in ('reg_buh','reg_buh_col','dim_period','dim_account') order by relname"
        echo "  parquet $PARQUET"
        echo "          $(stat -c '%s bytes' "$PARQUET")  md5=$(md5sum "$PARQUET" | cut -d' ' -f1)"
        echo "  s3      $S3URI at ${XPB_S3_ENDPOINT:-<unset in this shell>}"
        echo "          server ETag: $(s3_etag)"
        echo
        echo "-- zlfs zones"
        "${PSQL[@]}" -At -c "select '  ['||period_lo||'..'||period_hi||'] rows='||nrows||' size='||size_kb||' kB '||freshness from zlfs_zone_info()" 2>/dev/null
        echo
        echo "-- build flags of the Parquet module"
        grep -m1 "^CXXFLAGS\|^PG_CPPFLAGS" "$REPO/extension/xpb_parquet/Makefile" 2>/dev/null | sed 's/^/  /'
    } > "$f" 2>&1
    cat "$f"
}

# The object's identity as the server states it, for the remote arm's record.
s3_etag() {
    "${PSQL[@]}" -At -c "LOAD 'xpb_parquet'; select s3_identity from xpq_scan('$S3URI','period_key',1,0)" 2>/dev/null | tail -1
}

# ----------------------------------------------------------------- primitives
#
# One psql invocation = one backend. That is the unit of the newconn state, so
# the harness never reuses a connection across states by accident.

# Run a SQL file in ONE fresh backend with \timing on, capture everything.
run_file() {        # run_file <sqlfile> <outfile>
    "${PSQL[@]}" -f "$1" > "$2" 2>&1
}

# Pull the Nth "Time: X ms" from psql output.
nth_time() {        # nth_time <file> <n>
    grep -oP '^Time: \K[0-9.]+' "$1" | sed -n "${2}p"
}

# Pull the Nth stage NOTICE from psql output, stripped to its numbers.
#
# Anchored on total= rather than on "mode=...:" because the Parquet modes are
# URIs and contain colons: ext:parquet:s3://xpb/... would be cut in the wrong
# place. It happened to still work, since the numbers follow either way, which
# is the kind of accident that stops working when a label changes.
nth_stages() {      # nth_stages <file> <n>
    grep -oP 'total=\K.*' "$1" | sed -n "${2}p" | sed 's/^/total=/'
}

field() {           # field <stageline> <name>   e.g. field "$s" total
    echo "$1" | grep -oP "(^|\s)$2=\K[0-9.]+" | head -1
}

csv_row() {         # csv_row arm shape state run wall total source operators rows notes
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,"%s"\n' "$@" >> "$CSV"
}

# ------------------------------------------------------------- the join2 arms
#
# xpb_batch_join2_groupby is the ONE entry point every representation reaches,
# so it is the only shape in which all of them are comparable. It is a
# join+aggregate: two dimension joins and a group-by. "Full scan" and "pruning"
# are therefore measured as ranges WITHIN this shape, not as a separate scan
# shape -- a pure scan arm exists only for Parquet (xpq_scan) and SQL.
ARMS_NAME=(heap-xpb pgcolumnar-xpb zlfs-xpb parquet-local-xpb parquet-s3-xpb)
ARMS_MODE=(heap pgcolumnar zlfs "ext:parquet:$PARQUET" "ext:parquet:$S3URI")

# lo hi label expected_rows expected_groups
SHAPES=("1 120 full 10000000 2000" "25 36 selective 1000008 200" "25 25 narrow 83334 200")

join2_sql() {       # join2_sql <mode> <lo> <hi> <repeat>  -> prints a sql file path
    local mode="$1" lo="$2" hi="$3" rep="$4"
    local f="$RUNDIR/.q.sql"
    {
        echo "\\timing on"
        echo "SET client_min_messages=notice;"
        echo "LOAD 'xpb_parquet';"
        for ((i=0;i<rep;i++)); do
            echo "SELECT count(*) AS groups, sum(total_amt) AS checksum FROM xpb_batch_join2_groupby($lo,$hi,'$mode');"
        done
    } > "$f"
    echo "$f"
}

echo "############ Performance Baseline v1 ############"
echo
env_record
echo
echo "raw: $RUNDIR"
echo "csv: $CSV"
echo
echo "arm,shape,state,run,wall_ms,total_ms,source_ms,operators_ms,rows,notes" > "$CSV"

# ---- correctness gate FIRST: no timing from an arm that has not matched the oracle
echo "=== correctness gate: every arm against the PostgreSQL heap oracle ==="
gate_fail=0
for s in "${SHAPES[@]}"; do
    set -- $s; lo=$1; hi=$2; label=$3; want_rows=$4; want_groups=$5
    # The oracle is PostgreSQL over the heap, expressed in plain SQL with the
    # same semantics as the batch pipeline: two dimension joins, one group-by.
    oracle=$("${PSQL[@]}" -At -c "
        SELECT count(*)||'|'||sum(total_amt)||'|'||sum(n)
        FROM (SELECT p.year, a.account_group, f.company_key,
                     sum(f.amount_dt)::bigint AS total_amt, count(*) AS n
              FROM reg_buh f
              JOIN dim_period  p ON p.period_key  = f.period_key
              JOIN dim_account a ON a.account_key = f.account_key
              WHERE f.period_key BETWEEN $lo AND $hi
              GROUP BY 1,2,3) q" 2>&1 | tail -1)
    echo "  oracle [$lo..$hi] groups|checksum|rows = $oracle"
    for i in "${!ARMS_NAME[@]}"; do
        # Notices stay ON here: the group count and checksum come from the
        # result, the ROW COUNT only from the arm's own NOTICE, and a gate that
        # checks the aggregate but not the rows would pass an arm that read the
        # wrong number of rows and summed to the same total by cancellation.
        "${PSQL[@]}" -At -c "LOAD 'xpb_parquet'; SET client_min_messages=notice;
              SELECT count(*)||'|'||sum(total_amt) FROM xpb_batch_join2_groupby($lo,$hi,'${ARMS_MODE[$i]}')" \
              > "$RUNDIR/.g" 2>&1
        got=$(grep -oP '^[0-9]+\|[0-9-]+$' "$RUNDIR/.g" | tail -1)
        grows=$(grep -oP 'rows=\K[0-9]+' "$RUNDIR/.g" | tail -1)
        if [ "${oracle%|*}" = "$got" ] && [ "$grows" = "$want_rows" ]; then
            echo "  ok      ${ARMS_NAME[$i]} [$label] matches the oracle ($got, rows=$grows)"
        else
            echo "  FAIL    ${ARMS_NAME[$i]} [$label] oracle=${oracle%|*}/$want_rows arm=$got/${grows:-no-rows}"
            gate_fail=$((gate_fail+1))
        fi
    done
done

if [ "$gate_fail" -ne 0 ]; then
    echo
    echo "!! $gate_fail arm(s) failed the oracle. No timing is recorded for a"
    echo "   measurement whose answer is wrong."
    exit 1
fi

# ---- newconn and warm series
for s in "${SHAPES[@]}"; do
    set -- $s; lo=$1; hi=$2; label=$3; want_rows=$4
    echo
    echo "=== shape $label [$lo..$hi], expecting $want_rows rows ==="
    for i in "${!ARMS_NAME[@]}"; do
        arm="${ARMS_NAME[$i]}"; mode="${ARMS_MODE[$i]}"

        # newconn: ONE fresh backend, two calls. The first pays whatever this
        # representation sets up per backend; the second does not. Both are
        # recorded -- the pair IS the measurement of that setup cost.
        q=$(join2_sql "$mode" "$lo" "$hi" 2)
        out="$RUNDIR/$label-$arm-newconn.txt"
        { echo "# arm=$arm shape=$label range=[$lo..$hi] state=newconn commit=$COMMIT";
          echo "# command: psql -f <2 calls of xpb_batch_join2_groupby($lo,$hi,'$mode')>";
          echo; } > "$out"
        run_file "$q" "$RUNDIR/.o" ; cat "$RUNDIR/.o" >> "$out"
        w1=$(nth_time "$RUNDIR/.o" 3); w2=$(nth_time "$RUNDIR/.o" 4)
        s1=$(nth_stages "$RUNDIR/.o" 1); s2=$(nth_stages "$RUNDIR/.o" 2)
        csv_row "$arm" "$label" newconn 1 "${w1:-}" "$(field "$s1" total)" "$(field "$s1" source)" "$(field "$s1" agg)" "$(field "$s1" rows)" "first call in a fresh backend"
        csv_row "$arm" "$label" newconn 2 "${w2:-}" "$(field "$s2" total)" "$(field "$s2" source)" "$(field "$s2" agg)" "$(field "$s2" rows)" "second call, same backend"
        printf '  %-20s newconn  wall %8s -> %8s ms   (total %s -> %s)\n' \
               "$arm" "${w1:-?}" "${w2:-?}" "$(field "$s1" total)" "$(field "$s2" total)"

        # warm: one backend, 1 warm-up + NWARM measured.
        q=$(join2_sql "$mode" "$lo" "$hi" $((NWARM+1)))
        out="$RUNDIR/$label-$arm-warm.txt"
        { echo "# arm=$arm shape=$label range=[$lo..$hi] state=warm runs=$NWARM (after 1 warm-up) commit=$COMMIT";
          echo; } > "$out"
        run_file "$q" "$RUNDIR/.o" ; cat "$RUNDIR/.o" >> "$out"
        walls=""
        for ((r=1;r<=NWARM;r++)); do
            w=$(nth_time "$RUNDIR/.o" $((3+r)))        # skip \timing, SET, LOAD, warm-up
            st=$(nth_stages "$RUNDIR/.o" $((r+1)))
            csv_row "$arm" "$label" warm "$r" "${w:-}" "$(field "$st" total)" "$(field "$st" source)" "$(field "$st" agg)" "$(field "$st" rows)" ""
            walls="$walls ${w:-?}"
        done
        printf '  %-20s warm     wall%s ms\n' "$arm" "$walls"
    done
done

# ---------------------------------------------- Parquet-only scan shapes
#
# xpq_scan is the only entry point with a per-stage byte and request
# decomposition, and it exists for Parquet alone. Recorded as its own section
# rather than folded into a table with the other representations, which have no
# comparable counters.
echo
echo "=== Parquet scan shapes (xpq_scan; no equivalent exists for the others) ==="
PQ_SHAPES=("full:period_key,company_key,account_key,amount_dt::" \
           "projection:period_key::" \
           "pruned:period_key,company_key,account_key,amount_dt:25:36" \
           "pruned-to-nothing:period_key:1:0")
for src in "local:$PARQUET" "s3:$S3URI"; do
    sname="${src%%:*}"; spath="${src#*:}"
    for ps in "${PQ_SHAPES[@]}"; do
        IFS=':' read -r pl cols lo hi <<< "$ps"
        args="'$spath','$cols'"
        [ -n "$lo" ] && args="$args,$lo,$hi"
        out="$RUNDIR/pq-$sname-$pl.txt"
        { echo "# xpq_scan arm=$sname shape=$pl cols=$cols range=[${lo:-all}..${hi:-all}] commit=$COMMIT"; echo; } > "$out"
        { echo "\\timing on"; echo "SET client_min_messages=warning;"; echo "LOAD 'xpb_parquet';"
          for ((r=0;r<=NWARM;r++)); do
            echo "SELECT rows, read_calls, meta_calls, data_calls, meta_bytes, data_bytes,
                         bytes_requested, bytes_returned, logical_calls, logical_bytes,
                         row_groups_read, row_groups_skipped, decoded_values, decode_ms,
                         s3_get_calls, s3_http_attempts, s3_bytes_transferred, s3_retries,
                         s3_connections, s3_reconnects
                  FROM xpq_scan($args);"
          done; } > "$RUNDIR/.q.sql"
        run_file "$RUNDIR/.q.sql" "$RUNDIR/.o"; cat "$RUNDIR/.o" >> "$out"
        # SET is Time 1 and LOAD is Time 2, so the first CALL is Time 3. Reading
        # the first call as Time 2 reported the LOAD -- 0.1 ms -- as the scan,
        # and shifted every warm run one place earlier.
        w1=$(nth_time "$RUNDIR/.o" 3)
        wn=$(for ((r=1;r<=NWARM;r++)); do nth_time "$RUNDIR/.o" $((3+r)); done | tr '\n' ' ')
        # The per-request counters are this section's reason to exist, so they
        # belong in the summary and not only in the raw file. Taken from the
        # LAST warm row: they are identical across runs for a given shape, which
        # the raw file shows.
        vals=$(grep -P '^\s*[0-9]+ \|' "$RUNDIR/.o" | tail -1 | tr -d ' ')
        IFS='|' read -r c_rows c_readc c_metac c_datac c_metab c_datab c_req c_ret c_lcalls c_lbytes c_rgr c_rgs c_dec c_dms c_get c_att c_tx c_rty c_conn c_recon <<< "$vals"
        for ((r=1;r<=NWARM;r++)); do
            w=$(nth_time "$RUNDIR/.o" $((3+r)))
            csv_row "xpq_scan-$sname" "$pl" warm "$r" "${w:-}" "" "" "" "${c_rows:-}" \
                "reads=${c_readc:-} meta/data_calls=${c_metac:-}/${c_datac:-} bytes_req=${c_req:-} returned=${c_ret:-} logical=${c_lcalls:-}/${c_lbytes:-} rg_read=${c_rgr:-} rg_skip=${c_rgs:-} decoded=${c_dec:-} GET=${c_get:-} attempts=${c_att:-} transferred=${c_tx:-} retries=${c_rty:-} conns=${c_conn:-} reconn=${c_recon:-}"
        done
        csv_row "xpq_scan-$sname" "$pl" newconn 1 "${w1:-}" "" "" "" "${c_rows:-}" "first call in a fresh backend"
        printf '  %-6s %-18s first %8s ms   warm %s ms\n' "$sname" "$pl" "${w1:-?}" "$wn"
    done
done

echo
echo "raw artifacts: $RUNDIR"
echo "summary csv:   $CSV"
rm -f "$RUNDIR/.q.sql" "$RUNDIR/.o" "$RUNDIR/.g"
