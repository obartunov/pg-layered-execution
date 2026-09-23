#!/usr/bin/env python3
"""
Deterministic generator for dataset 1c-like-v2.

Same register shape as 1c-like-v1, with two changes that Benchmark 05-A needs.

1. CLUSTERED BY PERIOD.  Rows are emitted in period order -- all of period 1,
   then all of period 2, and so on.  1c-like-v1 assigned period round-robin
   (i % 12), which spreads every period across every pgColumnar row group and
   leaves nothing to prune; benchmark 04 measured rowgroups=7 with zero
   skipped because of exactly that.  Benchmark 05-A is about pruning, so the
   physical layout has to permit it.  The clustering is VERIFIED against
   pgcolumnar.zone_map by verify.sql, not assumed from insert order.

2. TYPED BATCH v2 ROW SHAPE.  The row carries a varlena and nullable columns,
   and the varlena sits at attnum 2 -- before every fixed-width column the
   pipeline reads.  That is the layout the fixed-offset source used to read at
   the wrong byte offsets, so it keeps the benchmark on the generic deform
   path where the heap is concerned.

The two money columns the report aggregates are int8 (`debit_cents`,
`credit_cents`).  The numeric columns (`debit`, `credit`) are separate
resources in the same row: they exercise the deform path and the numeric
accumulator, but no pgColumnar or ZLFS source can carry a numeric today, so
aggregating them would make the cross-path comparison impossible.  This is
stated in the README rather than hidden by quietly choosing integer columns.

Independence of the dimensions, the lesson from 1c-like-v1: company_key and
account_key are taken from two independent coordinates of the row's position
WITHIN its period, so the pair walks all 50 x 200 of the space and neither
determines the other.  verify.sql measures the pair count instead of trusting
this comment.
"""
import sys

PERIODS   = 12
ROWS      = 1_000_008          # 83_334 rows per period, exactly
COMPANIES = 50
ACCOUNTS  = 200
SEED      = 20260924

PER_PERIOD = ROWS // PERIODS

def main(out):
    out.write("period,comment,company_key,account_key,quantity,"
              "debit,credit,debit_cents,credit_cents\n")

    state = SEED
    for period in range(1, PERIODS + 1):
        for j in range(PER_PERIOD):
            # two independent coordinates of j: the pair covers all 50 x 200
            company = j % COMPANIES + 1
            account = (j // COMPANIES) % ACCOUNTS + 1

            # xorshift, so values do not correlate with the keys
            state ^= (state << 13) & 0xFFFFFFFFFFFFFFFF
            state ^= state >> 7
            state ^= (state << 17) & 0xFFFFFFFFFFFFFFFF

            # int8 money, the columns the report aggregates. Deliberately
            # past 2^31 in total so an int32 accumulator could not hold them.
            debit_cents = state % 1_000_000
            credit_cents = (state >> 20) % 1_000_000

            # account_key is int8 and past 2^31, so a source that truncated
            # to int32 would collapse distinct accounts together
            account_key = 4_000_000_000 + account

            # nullable int8: 1 row in 9
            quantity = "" if j % 9 == 0 else str((state >> 7) % 100_000)

            # numeric with a real fractional part; debit NULL 1 row in 6
            debit = "" if j % 6 == 0 else f"{(state % 97_700) / 100.0:.2f}"
            credit = f"{((state >> 11) % 38_300) / 100.0:.2f}"

            # varlena, NULL 1 row in 4 and empty 1 row in 4, so NULL and ''
            # stay distinguishable
            if j % 4 == 0:
                comment = ""            # written as NULL via the NULL marker
            elif j % 4 == 1:
                comment = '""'          # empty string, not NULL
            else:
                comment = "c" * (j % 37 + 1)

            out.write(f"{period},{comment},{company},{account_key},{quantity},"
                      f"{debit},{credit},{debit_cents},{credit_cents}\n")

if __name__ == "__main__":
    main(sys.stdout)
