#!/bin/bash
#
# Control: does patches/required/0001-executor-batch-api.patch change the
# ordinary row path at all?
#
#   benchmarks/04-1c-like-register/stock-vs-patched.sh \
#       <STOCK_PORT> <PATCHED_PORT> [PGHOST] [DBNAME]
#
# The same vanilla SQL, on the same dataset, on two servers built from the same
# PostgreSQL commit — one stock, one with the required patch applied — both
# running an ordinary PostgreSQL plan. Until this has run, the patched server's
# row path must not be called "vanilla PostgreSQL".
#
# The patch only adds ExecProcNodeBatch() and its fallback; nothing in these
# plans calls it. This measures that rather than asserting it.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STOCK_PORT="${1:?stock port}"
PATCHED_PORT="${2:?patched port}"
PGHOST_ARG="${3:-}"
DB="${4:-onec}"
RUNS=5

SQL="SELECT count(*) FROM (
       SELECT p.year, a.account_group, r.company_key,
              sum(r.amount_dt)::bigint,
              sum(r.amount_kt)::bigint,
              sum(r.amount_dt::bigint - r.amount_kt::bigint)
       FROM reg_buh r
       JOIN dim_period  p ON p.period_key  = r.period_key
       JOIN dim_account a ON a.account_key = r.account_key
       WHERE r.period_key BETWEEN 1 AND 12
       GROUP BY p.year, a.account_group, r.company_key) s;"

for port in "$STOCK_PORT" "$PATCHED_PORT"; do
    PSQL=(psql -p "$port" -d "$DB" -v ON_ERROR_STOP=1 -X -qAt)
    [ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")
    echo "-- port $port"
    "${PSQL[@]}" -c "SELECT 'server: ' || version()" | cut -c1-60
    for i in $(seq $RUNS); do
        "${PSQL[@]}" <<SQL | grep -i '^time'
SET max_parallel_workers_per_gather = 0;
SET jit = off;
\\timing on
$SQL
SQL
    done
done
