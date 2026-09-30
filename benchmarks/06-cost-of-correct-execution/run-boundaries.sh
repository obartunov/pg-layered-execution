#!/bin/bash
#
# Benchmark 06, part 2 — the two boundaries the main ladder cannot reach.
#
#   benchmarks/06-cost-of-correct-execution/run-boundaries.sh <PGPORT> [PGHOST] [DBNAME]
#
# D. Join hit rate. The report's dimension tables are complete, so every cell in
#    run-cost.sh joins at 100%. The hit rate is dialled here by deleting
#    dimension rows inside a transaction that is rolled back, so the committed
#    dataset is byte-identical afterwards and the fact table is never touched.
#    The row-flow counters report the achieved rate rather than the intended one.
#
# E. Refusal cost (section 10). After the Silent-Drop closure an Xp node either
#    implements the query or declines it. Declining is correct and it is not
#    free: the query is planned by PostgreSQL instead. Four shapes, each with
#    the Xp node enabled, so what is measured is the decision the planner
#    actually made -- reported by EXPLAIN alongside the time.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${1:-5432}"; PGHOST_ARG="${2:-}"; DB="${3:-onec2}"
REPS="${REPS:-9}"
RAW="$HERE/raw"; mkdir -p "$RAW"
: > "$RAW/hitrate.txt"   # truncate: these are appended per keep-level

PSQL=(psql -p "$PORT" -X -qAt -d "$DB")
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

echo "############ Benchmark 06 part 2 — join hit rate and refusal cost ############"

# ------------------------------------------------------------------ part D --
echo
echo "=== D. join hit rate (dimension rows deleted, transaction rolled back) ==="
echo "reg2 [1..12], heap-deform. flow_rows_* report the achieved rate."
echo

for keep in 100 50 10 1; do
  {
    echo "BEGIN;"
    [ "$keep" != 100 ] && \
      echo "DELETE FROM dim_company WHERE company_key % 100 >= $keep;"
    echo "SET client_min_messages=notice;"
    echo "SELECT count(*) FROM xpb_v2_register_report(1,12,'heap-deform');"
    echo "ROLLBACK;"
  } | "${PSQL[@]}" 2>&1 | grep '^NOTICE' | \
      sed "s/^/keep=${keep}% /" | tee -a "$RAW/hitrate.txt"
done

echo
echo "--- committed dataset unchanged after rollback ---"
"${PSQL[@]}" -c "SELECT 'dim_company rows: ' || count(*) FROM dim_company"

# ------------------------------------------------------------------ part E --
echo
echo "=== E. refusal cost: Xp accepted vs Xp refused ==="
echo

"${PSQL[@]}" >/dev/null 2>&1 <<'SQL'
DROP TABLE IF EXISTS rf_t;
CREATE TABLE rf_t (k1 int4 NOT NULL, k2 int4 NOT NULL, v int8 NOT NULL, tag text);
-- k1 = g/2000, not g%1000: 1000 groups either way, but the ascending form is
-- correlated with physical order and the modulo form is not. XpGroupAgg2 is
-- cost-gated and is NOT selected for the uncorrelated form at this size, so
-- the modulo table would have made every shape below "refused" for a cost
-- reason and measured nothing about refusal. This is a property of the cost
-- model, recorded here, not tuned.
INSERT INTO rf_t SELECT g/2000, 0, 1, CASE WHEN g%10=0 THEN 'keep' ELSE 'skip' END
  FROM generate_series(0, 1999999) g;
ANALYZE rf_t;
SQL

XPON="LOAD 'xp_batch'; SET xp_batch.enabled=on; SET xp_batch.pageagg=on;
      SET xp_batch.groupagg2=on; SET xp_batch.pageagg_summary=on;
      SET xp_batch.pageagg_skip=on; SET jit=off; SET max_parallel_workers_per_gather=0;"

# shape <label> <query>
shape() {
    local label="$1" q="$2" plan node t oracle xp
    plan=$("${PSQL[@]}" -c "$XPON EXPLAIN (COSTS OFF) $q" 2>&1)
    if grep -q 'Custom Scan (Xp' <<<"$plan"; then
        node=$(grep -o 'Custom Scan (Xp[A-Za-z0-9]*)' <<<"$plan" | head -1)
        node="accepted: $node"
    else
        node="refused -> $(sed -n '1p' <<<"$plan" | sed 's/^ *//')"
    fi

    # Correctness: the same query with every Xp node off is the oracle.
    # The output is SORTED before hashing. XpGroupAgg2 emits groups in hash
    # order and HashAggregate in its own; an order-sensitive checksum reports
    # a mismatch on two identical results, which it duly did before this.
    oracle=$("${PSQL[@]}" -c "SET xp_batch.enabled=off; SET jit=off;
                              SET max_parallel_workers_per_gather=0; $q" 2>&1 | sort | md5sum | cut -c1-12)
    xp=$("${PSQL[@]}" -c "$XPON $q" 2>&1 | sort | md5sum | cut -c1-12)
    [ "$oracle" = "$xp" ] && ck="ok" || ck="MISMATCH($oracle/$xp)"

    t=$({ echo "$XPON"; echo '\timing on';
          for i in $(seq 1 "$REPS"); do echo "$q;"; done; } | \
        "${PSQL[@]}" 2>&1 | grep '^Time:' | awk '{print $2}' | sort -n | \
        awk '{a[NR]=$1} END{printf "%.1f", (NR%2)?a[(NR+1)/2]:(a[NR/2]+a[NR/2+1])/2}')

    printf '%-34s %-46s %8s ms   result=%s\n' "$label" "$node" "$t" "$ck"
    printf '%s|%s|%s|%s\n' "$label" "$node" "$t" "$ck" >> "$RAW/refusal.txt"
}

: > "$RAW/refusal.txt"
shape "fully supported" \
      "SELECT k1,k2,sum(v) FROM rf_t WHERE k1 <= 500 GROUP BY k1,k2"
shape "one residual qual" \
      "SELECT k1,k2,sum(v) FROM rf_t WHERE k1 <= 500 AND tag = 'keep' GROUP BY k1,k2"
shape "unsupported target expression" \
      "SELECT 'x' || sum(v) FROM rf_t WHERE k1 <= 500"
shape "HAVING" \
      "SELECT k1,k2,sum(v) FROM rf_t WHERE k1 <= 500 GROUP BY k1,k2 HAVING sum(v) > 5"
shape "supported single aggregate" \
      "SELECT sum(v) FROM rf_t WHERE k1 <= 500"

"${PSQL[@]}" -c "DROP TABLE IF EXISTS rf_t" >/dev/null 2>&1
echo
echo "raw: $RAW/hitrate.txt  $RAW/refusal.txt"
