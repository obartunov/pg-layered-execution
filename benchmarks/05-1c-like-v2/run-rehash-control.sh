#!/bin/bash
#
# Rehash cost control, for Hash Aggregate Growth v1
#
#   benchmarks/05-1c-like-v2/run-rehash-control.sh <PGPORT> [PGHOST] [DBNAME]
#
# The growth ladder cannot separate "aggregating into a big table" from
# "getting there by doubling", because a run that ends at capacity C has also
# paid every rehash on the way. This isolates it.
#
# Both arms end with the IDENTICAL final table -- same capacity, same group
# count, same load factor, same probe distribution -- and differ only in how
# they arrived:
#
#   natural     starts at 16384, doubles five times, rehashing 253 952 groups
#   pre-sized   starts at the final capacity, zero growths, zero rehashing
#
# Pre-sizing uses the test-only policy hook. It is NOT how the pipeline behaves
# and is not a proposal: section 27 forbids sizing the table from a cardinality
# known in advance, precisely because it would hide the thing being measured.
# Here it is used as a control to measure exactly that hidden thing.
#
# The two arms alternate inside ONE connection, so the comparison is paired and
# immune to the between-session drift that makes cross-day timings useless on
# this host.
set -euo pipefail

PORT="${1:-5432}"
PGHOST_ARG="${2:-}"
DB="${3:-onec2}"
PAIRS=10

PSQL=(psql -p "$PORT" -d "$DB" -v ON_ERROR_STOP=1 -X -qAt)
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

GUC="SET work_mem='64MB'; SET enable_hashjoin=on; SET jit=off;
     SET max_parallel_workers_per_gather=0;"

# 192 x 768 = 147 456 groups, which the natural policy reaches at capacity
# 524288 after five growths.
"${PSQL[@]}" -c "SELECT xpe_set_cardinality_wide(768)" >/dev/null
"${PSQL[@]}" -c "SELECT zlfs_build_zone('reg2_card2','1,2,3,4,5',1,12)" >/dev/null 2>&1 || true

echo "# arm,pair,groups,capacity,growths,agg_ms,rehash_ms,max_probe_current,max_probe_lifetime"

one() {   # one <policy_sql> <arm> <pair>
    local line
    line=$("${PSQL[@]}" -c "$GUC $1
             SELECT count(*) FROM xpb_v2_register_report(1,12,'card2-zlfs')" 2>&1 \
           | grep -v '^WARNING' | sed -n 's/^NOTICE:  v2_register_report //p')
    g() { sed -n "s/.*[ =]$1=\([0-9.]*\).*/\1/p" <<<"$line"; }
    echo "$2,$3,$(g groups),$(g grp_cap),$(g grp_growths),$(g agg),$(g grp_rehash_ms),$(g grp_max_probe_current),$(g grp_max_probe_lifetime)"
}

# warm-up pair, discarded
one "SELECT xpb_grp_test_policy(0, 0);"      natural  0 >/dev/null
one "SELECT xpb_grp_test_policy(524288, 0);" presized 0 >/dev/null

for i in $(seq 1 $PAIRS); do
    one "SELECT xpb_grp_test_policy(0, 0);"      natural  "$i"
    one "SELECT xpb_grp_test_policy(524288, 0);" presized "$i"
done
