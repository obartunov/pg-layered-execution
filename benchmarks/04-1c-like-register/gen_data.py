#!/usr/bin/env python3
"""
Deterministic generator for dataset 1c-like-v1.

One closed annual section of an accounting register: 12 periods, ~1M rows,
50 companies, 200 accounts in 4 account groups. Seeded, so every machine and
every storage layer gets byte-identical rows.

Shape notes that matter for the benchmark, not for realism:

* company_key and account_key are taken from two INDEPENDENT coordinates of the
  row index -- i mod 50 and (i div 50) mod 200 -- so the pair walks the whole
  50x200 space and neither key determines the other. The first version of this
  generator advanced both with co-prime strides per row, which looks
  independent and is not: the pair then cycles with lcm(50,200) = 200, so only
  200 of the 10 000 pairs ever occur and each account belongs to exactly one
  company. verify.sql measures the pair count instead of trusting the
  construction, which is how that was caught.
* amount_dt and amount_kt are integers. int64 accumulation of them is exact,
  which is what makes the checksum comparable across paths. That is a property
  of this dataset; it is not support for PostgreSQL numeric.
* This is a deterministic synthetic distribution. It is NOT claimed to be
  realistic for 1C: no real register was available to model.
"""
import sys

PERIODS   = 12          # one year, period_key 1..12
ROWS      = 1_000_008   # divisible by 12 periods and by 8, keeps the split even
COMPANIES = 50
ACCOUNTS  = 200
SEED      = 20260923

def main(out):
    out.write("period_key,company_key,account_key,debit_key,credit_key,"
              "amount_dt,amount_kt,payload\n")

    state = SEED
    for i in range(ROWS):
        # three independent coordinates of i: periods stay exactly even, and
        # (company, account) covers all 50 x 200 pairs every 10 000 rows
        period  = i % PERIODS + 1
        company = i % COMPANIES + 1
        account = (i // COMPANIES) % ACCOUNTS + 1

        # xorshift, so the amounts do not correlate with the keys
        state ^= (state << 13) & 0xFFFFFFFFFFFFFFFF
        state ^= state >> 7
        state ^= (state << 17) & 0xFFFFFFFFFFFFFFFF

        amount_dt = state % 100_000
        amount_kt = (state >> 20) % 100_000
        debit_key = state % 100 + 1
        credit_key = (state >> 10) % 100 + 1
        payload = (state >> 30) % 1_000_000

        out.write(f"{period},{company},{account},{debit_key},{credit_key},"
                  f"{amount_dt},{amount_kt},{payload}\n")

if __name__ == "__main__":
    main(sys.stdout)
