#!/bin/bash
#
# Benchmark 06 — Cost of Correct Layered Execution
#
#   benchmarks/06-cost-of-correct-execution/run-cost.sh <PGPORT> [PGHOST] [DBNAME]
#
# This is a measurement run, not an optimisation run. Nothing here tunes
# anything; every cell reports what the pipeline already does.
#
# Two things make a cell valid:
#
#   1. The Xp result equals PostgreSQL's for the same question. A/B agreement
#      between two Xp arms is NOT accepted as correctness -- AGG_CAP and S2CAP
#      showed two arms dropping the same rows and keeping an equivalence gate
#      green. PostgreSQL is a mandatory third party (project rule since the
#      Silent-Drop closure).
#   2. Timing is taken only after that passes, from warm runs in ONE session.
#
# Session-scoped timing is deliberate. The ZLFS registry is built per backend,
# so a per-connection loop would charge every run for a directory scan that a
# real workload pays once. Both numbers are reported: the first run in a fresh
# backend is kept and labelled `cold`, the rest are the warm sample.
#
# Ladders:
#
#   A  reg2_fixed   fixed / deform / projected / early    one table, four paths
#   B  reg2 family  deform / projected / early / pgcolumnar / zlfs
#   C  card, card2, dimgrow                                group cardinality
#   D  reg2 with dimension rows deleted in a rolled-back transaction
#                                                          join hit rate
#
# Ladder A and ladder B are separate tables on purpose. reg2 carries a varlena
# at attnum 2, ahead of every column the pipeline reads, so the fixed-offset
# path cannot address it: `fixed` does not exist on reg2 and the dataset is not
# reshaped to manufacture it.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${1:-5432}"
PGHOST_ARG="${2:-}"
DB="${3:-onec2}"
REPS="${REPS:-11}"          # 1 cold + (REPS-1) warm, all in one session
RAW="$HERE/raw"

PSQL=(psql -p "$PORT" -X -qAt -d "$DB")
[ -n "$PGHOST_ARG" ] && PSQL+=(-h "$PGHOST_ARG")

mkdir -p "$RAW"
CELLS="$RAW/cells.csv"
NOTICES="$RAW/notices.txt"
GATE="$RAW/oracle.txt"
: > "$NOTICES"; : > "$GATE"
echo "ladder,mode,fact,lo,hi,rep,phase,total_ms" > "$CELLS"

GUC="SET work_mem='64MB'; SET jit=off; SET max_parallel_workers_per_gather=0;"

# ---------------------------------------------------------------- mode map --
# Mirrors the prefix dispatch in xpb_v2_report.c:959-1001. Kept here explicitly
# so the oracle queries the same table and the same dimensions the C code does;
# a mismatch here would silently compare two different questions.
fact_of() {
    case "$1" in
        pgcolumnar)   echo reg2_col ;;
        bad*)         echo reg2_bad ;;
        fixedlayout*) echo reg2_fixed ;;
        dimgrow*)     echo reg2_dim ;;
        card2*)       echo reg2_card2 ;;
        card*)        echo reg2_card ;;
        *)            echo reg2 ;;
    esac
}
dims_of() {
    case "$1" in
        dimgrow*) echo "dim_company_d dim_account_d" ;;
        card2*)   echo "dim_company_c2 dim_account_c2" ;;
        card-*|card) echo "dim_company_c dim_account_c" ;;
        *)        echo "dim_company dim_account2" ;;
    esac
}

# Canonical checksum over every grouping key and every aggregate, sorted before
# hashing so output order cannot enter it. total_ms is excluded by construction.
CK_XP="md5(coalesce(string_agg(company_group||','||account_group||','||company_key||','||
        debit_turnover||','||credit_turnover||','||net_turnover,
        '|' ORDER BY company_group, account_group, company_key),''))"

oracle_sql() {   # oracle_sql <fact> <dim1> <dim2> <lo> <hi>
cat <<SQL
WITH q AS (
  SELECT c.company_group, a.account_group, r.company_key,
         sum(r.debit_cents)::bigint  AS debit_turnover,
         sum(r.credit_cents)::bigint AS credit_turnover,
         sum(r.debit_cents - r.credit_cents)::bigint AS net_turnover
  FROM $1 r
  JOIN $2 c ON c.company_key = r.company_key
  JOIN $3 a ON a.account_key = r.account_key
  WHERE r.period BETWEEN $4 AND $5
  GROUP BY 1,2,3)
SELECT $CK_XP || '|' || count(*) FROM q;
SQL
}

xp_sql() {       # xp_sql <mode> <lo> <hi>
cat <<SQL
WITH q AS (SELECT * FROM xpb_v2_register_report($2,$3,'$1'))
SELECT $CK_XP || '|' || count(*) FROM q;
SQL
}

pass=0; fail=0; skip=0

# run_cell <ladder> <mode> <lo> <hi>
run_cell() {
    local ladder="$1" mode="$2" lo="$3" hi="$4"
    local fact dims d1 d2 want got

    fact=$(fact_of "$mode"); dims=$(dims_of "$mode")
    d1=${dims% *}; d2=${dims#* }

    want=$("${PSQL[@]}" -c "$GUC $(oracle_sql "$fact" "$d1" "$d2" "$lo" "$hi")" 2>&1 | tail -1)
    gotraw=$("${PSQL[@]}" -c "$GUC SET client_min_messages=warning; $(xp_sql "$mode" "$lo" "$hi")" 2>&1)
    got=$(tail -1 <<<"$gotraw")

    if [ "$want" != "$got" ]; then
        # A cell that cannot be built at all (no ZLFS zone for this range) is a
        # gap in coverage, not a wrong answer; it is reported as such. The test
        # is against the whole output: the ERROR line is followed by a HINT, so
        # the last line alone does not carry the diagnostic.
        if grep -q "no VALID ZLFS zone" <<<"$gotraw"; then
            printf '  SKIP  %-26s %-14s [%s..%s]  no zone for this range\n' \
                   "$mode" "$fact" "$lo" "$hi" | tee -a "$GATE"
            skip=$((skip+1)); return
        fi
        printf '  FAIL  %-26s %-14s [%s..%s]\n        want %s\n        got  %s\n' \
               "$mode" "$fact" "$lo" "$hi" "$want" "$got" | tee -a "$GATE"
        fail=$((fail+1)); return
    fi
    printf '  ok    %-26s %-14s [%s..%s]  %s\n' \
           "$mode" "$fact" "$lo" "$hi" "$want" | tee -a "$GATE"
    pass=$((pass+1))

    # Timing: one fresh backend, rep 1 cold, the rest warm. The NOTICE of the
    # last rep carries the stage split and the row-flow counters.
    {
        echo "$GUC"
        for i in $(seq 1 "$REPS"); do
            echo "SELECT 'T' || $i || ' ' || round(max(total_ms)::numeric,2) FROM xpb_v2_register_report($lo,$hi,'$mode');"
        done
    } | "${PSQL[@]}" 2>&1 | while IFS= read -r line; do
        case "$line" in
            T*)  set -- $line
                 rep=${1#T}
                 [ "$rep" = 1 ] && phase=cold || phase=warm
                 echo "$ladder,$mode,$fact,$lo,$hi,$rep,$phase,$2" >> "$CELLS" ;;
            NOTICE*) echo "$ladder|$mode|$fact|$lo|$hi| $line" >> "$NOTICES" ;;
        esac
    done
}

echo "############ Benchmark 06 — Cost of Correct Layered Execution ############"
echo
echo "=== versions ==="
"${PSQL[@]}" -c "SELECT 'server: ' || version()" | cut -c1-58
"${PSQL[@]}" -c "SELECT 'pgcolumnar: ' || extversion FROM pg_extension WHERE extname='pgcolumnar'"
"${PSQL[@]}" -c "SELECT 'zone files: ' || count(*) FROM pg_ls_dir('zlfs')" 2>/dev/null \
  || echo "zone files: (pg_ls_dir unavailable)"
echo "reps per cell: $REPS (rep 1 cold, rest warm), one session per cell"

echo
echo "=== correctness gate: every cell against PostgreSQL ==="
echo "Timing for a cell is taken only if its gate line says ok."
echo

RANGES="1:12 1:6 1:3 1:1 90:91"

echo "--- ladder A: heap paths, one table (reg2_fixed) ---"
for r in $RANGES; do
  for m in fixedlayout-fixed fixedlayout-deform fixedlayout-projected fixedlayout-early; do
    run_cell A "$m" "${r%:*}" "${r#*:}"
  done
done

echo "--- ladder B: storage paths (reg2 / reg2_col / ZLFS) ---"
for r in $RANGES; do
  for m in heap-deform heap-projected heap-early pgcolumnar zlfs; do
    run_cell B "$m" "${r%:*}" "${r#*:}"
  done
done

echo "--- ladder C: group cardinality, full range ---"
for m in heap-deform card card2 dimgrow card-zlfs card2-zlfs; do
  run_cell C "$m" 1 12
done

echo
echo "=== gate summary: $pass ok, $fail wrong, $skip not applicable ==="
[ "$fail" -gt 0 ] && echo "!! timings below are incomplete: $fail cell(s) disagreed with PostgreSQL"

echo
echo "raw: $CELLS"
echo "     $NOTICES"
echo "     $GATE"
exit $(( fail > 0 ? 1 : 0 ))
